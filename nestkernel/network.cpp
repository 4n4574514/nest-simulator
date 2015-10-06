/*
 *  network.cpp
 *
 *  This file is part of NEST.
 *
 *  Copyright (C) 2004 The NEST Initiative
 *
 *  NEST is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  NEST is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with NEST.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "instance.h"
#include "network.h"
#include "genericmodel.h"
#include "subnet.h"
#include "sibling_container.h"
#include "interpret.h"
#include "dict.h"
#include "dictstack.h"
#include "integerdatum.h"
#include "booldatum.h"
#include "doubledatum.h"
#include "dictutils.h"
#include "tokenutils.h"
#include "tokenarray.h"
#include "exceptions.h"
#include "sliexceptions.h"
#include "processes.h"
#include "nestmodule.h"
#include "sibling_container.h"
#include "communicator_impl.h"
#include "random_datums.h"

#include "kernel_manager.h"
#include "vp_manager_impl.h"
#include "connection_builder_manager_impl.h"

#include "nest_timeconverter.h"

#include <cmath>
#include <sys/time.h>
#include <set>
#ifdef _OPENMP
#include <omp.h>
#endif

#ifdef N_DEBUG
#undef N_DEBUG
#endif

#ifdef USE_PMA
#ifdef IS_K
extern PaddedPMA poormansallocpool[];
#else
extern PoorMansAllocator poormansallocpool;
#ifdef _OPENMP
#pragma omp threadprivate( poormansallocpool )
#endif
#endif
#endif

extern int SLIsignalflag;

namespace nest
{

Network* Network::network_instance_ = NULL;
bool Network::created_network_instance_ = false;

void
Network::create_network( SLIInterpreter& i )
{
#pragma omp critical( create_network )
  {
    if ( !created_network_instance_ )
    {
      network_instance_ = new Network( i );
      assert( network_instance_ );
      created_network_instance_ = true;
    }
  }
}

void
Network::destroy_network()
{
  delete network_instance_;
}


Network::Network( SLIInterpreter& i )
  : interpreter_( i )
  , connection_manager_()
  , root_( 0 )
  , current_( 0 )
  , dict_miss_is_error_( true )
  , model_defaults_modified_( false )
  , initialized_( false ) // scheduler stuff
  , n_gsd_( 0 )
  , nodes_vec_()
  , nodes_vec_network_size_( 0 ) // zero to force update
{
  // the subsequent function-calls need a
  // network instance, hence the instance
  // need to be set here
  // e.g. constructor of GenericModel, ConnectionManager -> get_num_threads()
  //
  network_instance_ = this;
  created_network_instance_ = true;

  kernel().init();

  init_scheduler_();

  modeldict_ = new Dictionary();
  interpreter_.def( "modeldict", new DictionaryDatum( modeldict_ ) );

  Model* model = new GenericModel< Subnet >( "subnet" );
  register_basis_model( *model );
  model->set_type_id( 0 );

  siblingcontainer_model = new GenericModel< SiblingContainer >( "siblingcontainer" );
  register_basis_model( *siblingcontainer_model, true );
  siblingcontainer_model->set_type_id( 1 );

  model = new GenericModel< proxynode >( "proxynode" );
  register_basis_model( *model, true );
  model->set_type_id( 2 );

  synapsedict_ = new Dictionary();
  interpreter_.def( "synapsedict", new DictionaryDatum( synapsedict_ ) );
  connection_manager_.init( synapsedict_ );

  interpreter_.def(
    "connruledict", new DictionaryDatum( kernel().connection_builder_manager.get_connruledict() ) );

  init_();
}

Network::~Network()
{
  destruct_nodes_();     // We must destruct nodes properly, since devices may need to close files.
  clear_models_( true ); // mark call from destructor

  // Now we can delete the clean model prototypes
  vector< std::pair< Model*, bool > >::iterator i;
  for ( i = pristine_models_.begin(); i != pristine_models_.end(); ++i )
    if ( ( *i ).first != 0 )
      delete ( *i ).first;

  initialized_ = false;
}

void
Network::init_()
{
  /*
   * We initialise the network with one subnet that is the root of the tree.
   * Note that we MUST NOT call add_node(), since it expects a properly
   * initialized network.
   */
  local_nodes_.reserve( 1 );

  SiblingContainer* root_container =
    static_cast< SiblingContainer* >( siblingcontainer_model->allocate( 0 ) );
  local_nodes_.add_local_node( *root_container );
  root_container->reserve( kernel().vp_manager.get_num_threads() );
  root_container->set_model_id( -1 );

  assert( !pristine_models_.empty() );
  Model* rootmodel = pristine_models_[ 0 ].first;
  assert( rootmodel != 0 );

  for ( index t = 0; t < kernel().vp_manager.get_num_threads(); ++t )
  {
    Node* newnode = rootmodel->allocate( t );
    newnode->set_gid_( 0 );
    newnode->set_model_id( 0 );
    newnode->set_thread( t );
    newnode->set_vp( kernel().vp_manager.thread_to_vp( t ) );
    root_container->push_back( newnode );
  }

  current_ = root_ = static_cast< Subnet* >( ( *root_container ).get_thread_sibling_( 0 ) );

  /**
    Build modeldict, list of models and list of proxy nodes from clean prototypes.
   */

  // Re-create the model list from the clean prototypes
  for ( index i = 0; i < pristine_models_.size(); ++i )
    if ( pristine_models_[ i ].first != 0 )
    {
      std::string name = pristine_models_[ i ].first->get_name();
      models_.push_back( pristine_models_[ i ].first->clone( name ) );
      if ( !pristine_models_[ i ].second )
        modeldict_->insert( name, i );
    }

  int proxy_model_id = get_model_id( "proxynode" );
  assert( proxy_model_id > 0 );
  Model* proxy_model = models_[ proxy_model_id ];
  assert( proxy_model != 0 );

  // create proxy nodes, one for each thread and model
  // create dummy spike sources, one for each thread
  proxy_nodes_.resize( kernel().vp_manager.get_num_threads() );
  for ( index t = 0; t < kernel().vp_manager.get_num_threads(); ++t )
  {
    for ( index i = 0; i < pristine_models_.size(); ++i )
    {
      if ( pristine_models_[ i ].first != 0 )
      {
        Node* newnode = proxy_model->allocate( t );
        newnode->set_model_id( i );
        proxy_nodes_[ t ].push_back( newnode );
      }
    }
    Node* newnode = proxy_model->allocate( t );
    newnode->set_model_id( proxy_model_id );
    dummy_spike_sources_.push_back( newnode );
  }

#ifdef HAVE_MUSIC
  music_in_portlist_.clear();
#endif
}

void
Network::init_scheduler_()
{
  assert( initialized_ == false );

  // explicitly force construction of nodes_vec_ to ensure consistent state
  update_nodes_vec_();


  initialized_ = true;
}

void
Network::destruct_nodes_()
{
  // We call the destructor for each node excplicitly. This destroys
  // the objects without releasing their memory. Since the Memory is
  // owned by the Model objects, we must not call delete on the Node
  // objects!
  for ( size_t n = 0; n < local_nodes_.size(); ++n )
  {
    Node* node = local_nodes_.get_node_by_index( n );
    assert( node != 0 );
    for ( size_t t = 0; t < node->num_thread_siblings_(); ++t )
      node->get_thread_sibling_( t )->~Node();
    node->~Node();
  }

  local_nodes_.clear();

  proxy_nodes_.clear();
  dummy_spike_sources_.clear();
}

void
Network::clear_models_( bool called_from_destructor )
{
  // no message on destructor call, may come after MPI_Finalize()
  if ( not called_from_destructor )
    LOG( M_INFO, "Network::clear_models", "Models will be cleared and parameters reset." );

  // We delete all models, which will also delete all nodes. The
  // built-in models will be recovered from the pristine_models_ in
  // init_()
  for ( vector< Model* >::iterator m = models_.begin(); m != models_.end(); ++m )
    if ( *m != 0 )
      delete *m;

  models_.clear();
  modeldict_->clear();
  model_defaults_modified_ = false;
}

void
Network::reset()
{
  kernel().reset();

  destruct_nodes_();
  clear_models_();

  // We free all Node memory and set the number of threads.
  vector< std::pair< Model*, bool > >::iterator m;
  for ( m = pristine_models_.begin(); m != pristine_models_.end(); ++m )
  {
    // delete all nodes, because cloning the model may have created instances.
    ( *m ).first->clear();
    ( *m ).first->set_threads();
  }

  initialized_ = false;
  kernel().init();
  init_scheduler_();

  connection_manager_.reset();

  init_();
}

void
Network::reset_kernel()
{
  /*
   * TODO: reset() below mixes both destruction of old nodes and
   * configuration of the fresh kernel. set_num_rec_processes() chokes
   * on this, as it expects a kernel without nodes. We now suppress that
   * test manually. Ideally, though, we should split reset() into one
   * part deleting all the old stuff, then perform settings for the
   * fresh kernel, then do remaining initialization.
   */
  kernel().vp_manager.set_num_threads( 1 );
  kernel().mpi_manager.set_num_rec_processes( 0, true );
  dict_miss_is_error_ = true;

  reset();
}


int
Network::get_model_id( const char name[] ) const
{
  const std::string model_name( name );
  for ( int i = 0; i < ( int ) models_.size(); ++i )
  {
    assert( models_[ i ] != NULL );
    if ( model_name == models_[ i ]->get_name() )
      return i;
  }
  return -1;
}


index Network::add_node( index mod, long_t n ) // no_p
{
  assert( current_ != 0 );
  assert( root_ != 0 );

  if ( mod >= models_.size() )
    throw UnknownModelID( mod );

  if ( n < 1 )
    throw BadProperty();

  const thread n_threads = kernel().vp_manager.get_num_threads();
  assert( n_threads > 0 );

  const index min_gid = local_nodes_.get_max_gid() + 1;
  const index max_gid = min_gid + n;

  Model* model = models_[ mod ];
  assert( model != 0 );

  /* current_ points to the instance of the current subnet on thread 0.
     The following code makes subnet a pointer to the wrapper container
     containing the instances of the current subnet on all threads.
   */
  const index subnet_gid = current_->get_gid();
  Node* subnet_node = local_nodes_.get_node_by_gid( subnet_gid );
  assert( subnet_node != 0 );

  SiblingContainer* subnet_container = dynamic_cast< SiblingContainer* >( subnet_node );
  assert( subnet_container != 0 );
  assert( subnet_container->num_thread_siblings_() == static_cast< size_t >( n_threads ) );
  assert( subnet_container->get_thread_sibling_( 0 ) == current_ );

  if ( max_gid > local_nodes_.max_size() || max_gid < min_gid )
  {
    LOG( M_ERROR, "Network::add:node", "Requested number of nodes will overflow the memory." );
    LOG( M_ERROR, "Network::add:node", "No nodes were created." );
    throw KernelException( "OutOfMemory" );
  }
  kernel().modelrange_manager.add_range( mod, min_gid, max_gid - 1 );

  if ( model->potential_global_receiver() and kernel().mpi_manager.get_num_rec_processes() > 0 )
  {
    // In this branch we create nodes for all GIDs which are on a local thread
    const int n_per_process = n / kernel().mpi_manager.get_num_rec_processes();
    const int n_per_thread = n_per_process / n_threads + 1;

    // We only need to reserve memory on the ranks on which we
    // actually create nodes. In this if-branch ---> Only on recording
    // processes
    if ( kernel().mpi_manager.get_rank() >= kernel().mpi_manager.get_num_sim_processes() )
    {
      local_nodes_.reserve( std::ceil(
        static_cast< double >( max_gid ) / kernel().mpi_manager.get_num_sim_processes() ) );
      for ( thread t = 0; t < n_threads; ++t )
      {
        // Model::reserve() reserves memory for n ADDITIONAL nodes on thread t
        model->reserve_additional( t, n_per_thread );
      }
    }

    for ( size_t gid = min_gid; gid < max_gid; ++gid )
    {
      const thread vp = kernel().vp_manager.suggest_rec_vp( get_n_gsd() );
      const thread t = kernel().vp_manager.vp_to_thread( vp );

      if ( kernel().vp_manager.is_local_vp( vp ) )
      {
        Node* newnode = model->allocate( t );
        newnode->set_gid_( gid );
        newnode->set_model_id( mod );
        newnode->set_thread( t );
        newnode->set_vp( vp );
        newnode->set_has_proxies( true );
        newnode->set_local_receiver( false );

        local_nodes_.add_local_node( *newnode ); // put into local nodes list

        current_->add_node( newnode ); // and into current subnet, thread 0.
      }
      else
      {
        local_nodes_.add_remote_node( gid ); // ensures max_gid is correct
        current_->add_remote_node( gid, mod );
      }
      increment_n_gsd();
    }
  }

  else if ( model->has_proxies() )
  {
    // In this branch we create nodes for all GIDs which are on a local thread
    const int n_per_process = n / kernel().mpi_manager.get_num_sim_processes();
    const int n_per_thread = n_per_process / n_threads + 1;

    // We only need to reserve memory on the ranks on which we
    // actually create nodes. In this if-branch ---> Only on
    // simulation processes
    if ( kernel().mpi_manager.get_rank() < kernel().mpi_manager.get_num_sim_processes() )
    {
      // TODO: This will work reasonably for round-robin. The extra 50 entries are
      //       for subnets and devices.
      local_nodes_.reserve( std::ceil( static_cast< double >( max_gid )
                              / kernel().mpi_manager.get_num_sim_processes() ) + 50 );
      for ( thread t = 0; t < n_threads; ++t )
      {
        // Model::reserve() reserves memory for n ADDITIONAL nodes on thread t
        // reserves at least one entry on each thread, nobody knows why
        model->reserve_additional( t, n_per_thread );
      }
    }

    for ( size_t gid = min_gid; gid < max_gid; ++gid )
    {
      const thread vp = kernel().vp_manager.suggest_vp( gid );
      const thread t = kernel().vp_manager.vp_to_thread( vp );

      if ( kernel().vp_manager.is_local_vp( vp ) )
      {
        Node* newnode = model->allocate( t );
        newnode->set_gid_( gid );
        newnode->set_model_id( mod );
        newnode->set_thread( t );
        newnode->set_vp( vp );

        local_nodes_.add_local_node( *newnode ); // put into local nodes list
        current_->add_node( newnode );           // and into current subnet, thread 0.
      }
      else
      {
        local_nodes_.add_remote_node( gid ); // ensures max_gid is correct
        current_->add_remote_node( gid, mod );
      }
    }
  }
  else if ( !model->one_node_per_process() )
  {
    // We allocate space for n containers which will hold the threads
    // sorted. We use SiblingContainers to store the instances for
    // each thread to exploit the very efficient memory allocation for
    // nodes.
    //
    // These containers are registered in the global nodes_ array to
    // provide access to the instances both for manipulation by SLI
    // functions and so that Network::calibrate() can discover the
    // instances and register them for updating.
    //
    // The instances are also registered with the instance of the
    // current subnet for the thread to which the created instance
    // belongs. This is mainly important so that the subnet structure
    // is preserved on all VPs.  Node enumeration is done on by the
    // registration with the per-thread instances.
    //
    // The wrapper container can be addressed under the GID assigned
    // to no-proxy node created. If this no-proxy node is NOT a
    // container (e.g. a device), then each instance can be retrieved
    // by giving the respective thread-id to get_node(). Instances of
    // SiblingContainers cannot be addressed individually.
    //
    // The allocation of the wrapper containers is spread over threads
    // to balance memory load.
    size_t container_per_thread = n / n_threads + 1;

    // since we create the n nodes on each thread, we reserve the full load.
    for ( thread t = 0; t < n_threads; ++t )
    {
      model->reserve_additional( t, n );
      siblingcontainer_model->reserve_additional( t, container_per_thread );
      static_cast< Subnet* >( subnet_container->get_thread_sibling_( t ) )->reserve( n );
    }

    // The following loop creates n nodes. For each node, a wrapper is created
    // and filled with one instance per thread, in total n * n_thread nodes in
    // n wrappers.
    local_nodes_.reserve( std::ceil( static_cast< double >( max_gid )
                            / kernel().mpi_manager.get_num_sim_processes() ) + 50 );
    for ( index gid = min_gid; gid < max_gid; ++gid )
    {
      thread thread_id = kernel().vp_manager.vp_to_thread( kernel().vp_manager.suggest_vp( gid ) );

      // Create wrapper and register with nodes_ array.
      SiblingContainer* container =
        static_cast< SiblingContainer* >( siblingcontainer_model->allocate( thread_id ) );
      container->set_model_id(
        -1 ); // mark as pseudo-container wrapping replicas, see reset_network()
      container->reserve( n_threads ); // space for one instance per thread
      container->set_gid_( gid );
      local_nodes_.add_local_node( *container );

      // Generate one instance of desired model per thread
      for ( thread t = 0; t < n_threads; ++t )
      {
        Node* newnode = model->allocate( t );
        newnode->set_gid_( gid ); // all instances get the same global id.
        newnode->set_model_id( mod );
        newnode->set_thread( t );
        newnode->set_vp( kernel().vp_manager.thread_to_vp( t ) );

        // Register instance with wrapper
        // container has one entry for each thread
        container->push_back( newnode );

        // Register instance with per-thread instance of enclosing subnet.
        static_cast< Subnet* >( subnet_container->get_thread_sibling_( t ) )->add_node( newnode );
      }
    }
  }
  else
  {
    // no proxies and one node per process
    // this is used by MUSIC proxies
    // Per r9700, this case is only relevant for music_*_proxy models,
    // which have a single instance per MPI process.
    for ( index gid = min_gid; gid < max_gid; ++gid )
    {
      Node* newnode = model->allocate( 0 );
      newnode->set_gid_( gid );
      newnode->set_model_id( mod );
      newnode->set_thread( 0 );
      newnode->set_vp( kernel().vp_manager.thread_to_vp( 0 ) );

      // Register instance
      local_nodes_.add_local_node( *newnode );

      // and into current subnet, thread 0.
      current_->add_node( newnode );
    }
  }

  // set off-grid spike communication if necessary
  if ( model->is_off_grid() )
  {
    kernel().event_delivery_manager.set_off_grid_communication( true );
    LOG( M_INFO,
      "network::add_node",
      "Neuron models emitting precisely timed spikes exist: "
      "the kernel property off_grid_spiking has been set to true.\n\n"
      "NOTE: Mixing precise-spiking and normal neuron models may "
      "lead to inconsistent results." );
  }

  return max_gid - 1;
}

void
Network::restore_nodes( const ArrayDatum& node_list )
{
  Subnet* root = get_cwn();
  const index gid_offset = size() - 1;
  Token* first = node_list.begin();
  const Token* end = node_list.end();
  if ( first == end )
    return;

  // We need to know the first and hopefully smallest GID to identify
  // if a parent is in or outside the range of restored nodes.
  // So we retrieve it here, from the first element of the node_list, assuming that
  // the node GIDs are in ascending order.
  DictionaryDatum node_props = getValue< DictionaryDatum >( *first );
  const index min_gid = ( *node_props )[ names::global_id ];

  for ( Token* node_t = first; node_t != end; ++node_t )
  {
    DictionaryDatum node_props = getValue< DictionaryDatum >( *node_t );
    std::string model_name = ( *node_props )[ names::model ];
    index model_id = get_model_id( model_name.c_str() );
    index parent_gid = ( *node_props )[ names::parent ];
    index local_parent_gid = parent_gid;
    if ( parent_gid >= min_gid )      // if the parent is one of the restored nodes
      local_parent_gid += gid_offset; // we must add the gid_offset
    go_to( local_parent_gid );
    index node_gid = add_node( model_id );
    Node* node_ptr = get_node( node_gid );
    // we call directly set_status on the node
    // to bypass checking of unused dictionary items.
    node_ptr->set_status_base( node_props );
  }
  current_ = root;
}

void
Network::init_state( index GID )
{
  Node* n = get_node( GID );
  if ( n == 0 )
    throw UnknownNode( GID );

  n->init_state();
}

void
Network::go_to( index n )
{
  if ( Subnet* target = dynamic_cast< Subnet* >( get_node( n ) ) )
    current_ = target;
  else
    throw SubnetExpected();
}

Node* Network::get_node( index n, thread thr ) // no_p
{
  Node* node = local_nodes_.get_node_by_gid( n );
  if ( node == 0 )
  {
    return proxy_nodes_[ thr ].at( kernel().modelrange_manager.get_model_id( n ) );
  }

  if ( node->num_thread_siblings_() == 0 )
    return node; // plain node

  if ( thr < 0 || thr >= static_cast< thread >( node->num_thread_siblings_() ) )
    throw UnknownNode();

  return node->get_thread_sibling_( thr );
}

const SiblingContainer*
Network::get_thread_siblings( index n ) const
{
  Node* node = local_nodes_.get_node_by_gid( n );
  if ( node->num_thread_siblings_() == 0 )
    throw NoThreadSiblingsAvailable( n );
  const SiblingContainer* siblings = dynamic_cast< SiblingContainer* >( node );
  assert( siblings != 0 );

  return siblings;
}

void
Network::memory_info()
{
  std::cout.setf( std::ios::left );
  std::vector< index > idx( models_.size() );

  for ( index i = 0; i < models_.size(); ++i )
    idx[ i ] = i;

  std::sort( idx.begin(), idx.end(), ModelComp( models_ ) );

  std::string sep( "--------------------------------------------------" );

  std::cout << sep << std::endl;
  std::cout << std::setw( 25 ) << "Name" << std::setw( 13 ) << "Capacity" << std::setw( 13 )
            << "Available" << std::endl;
  std::cout << sep << std::endl;

  for ( index i = 0; i < models_.size(); ++i )
  {
    Model* mod = models_[ idx[ i ] ];
    if ( mod->mem_capacity() != 0 )
      std::cout << std::setw( 25 ) << mod->get_name() << std::setw( 13 )
                << mod->mem_capacity() * mod->get_element_size() << std::setw( 13 )
                << mod->mem_available() * mod->get_element_size() << std::endl;
  }

  std::cout << sep << std::endl;
  std::cout.unsetf( std::ios::left );
}

void
Network::print( index p, int depth )
{
  Subnet* target = dynamic_cast< Subnet* >( get_node( p ) );
  if ( target != NULL )
    std::cout << target->print_network( depth + 1, 0 );
  else
    throw SubnetExpected();
}

void
Network::set_status( index gid, const DictionaryDatum& d )
{
  // we first handle normal nodes, except the root (GID 0)
  if ( gid > 0 )
  {
    Node* target = local_nodes_.get_node_by_gid( gid );
    if ( target != 0 )
    {
      // node is local
      if ( target->num_thread_siblings_() == 0 )
        set_status_single_node_( *target, d );
      else
        for ( size_t t = 0; t < target->num_thread_siblings_(); ++t )
        {
          // non-root container for devices without proxies and subnets
          // we iterate over all threads
          assert( target->get_thread_sibling_( t ) != 0 );
          set_status_single_node_( *( target->get_thread_sibling_( t ) ), d );
        }
    }
    return;
  }

  /* Code below is executed only for the root node, gid == 0

     In this case, we must
     - set scheduler properties
     - set properties for the compound representing each thread

     The main difficulty here is to handle the access control for
     dictionary items, since the dictionary is read in several places.

     We proceed as follows:
     - clear access flags
     - set scheduler properties; this must be first, anyways
     - at this point, all non-compound property flags are marked accessed
     - loop over all per-thread compounds
     - the first per-thread compound will flag all compound properties as read
     - now, all dictionary entries must be flagged as accessed, otherwise the
       dictionary contains unknown entries. Thus, set_status_single_node_
       will not throw an exception
     - since all items in the root node are of type Compound, all read the same
       properties and we can leave the access flags set
   */
  d->clear_access_flags();

  // former scheduler_.set_status( d ); start
  // careful, this may invalidate all node pointers!
  assert( initialized_ );

  kernel().set_status( d );


  // former scheduler_.set_status( d ); end

  updateValue< bool >( d, "dict_miss_is_error", dict_miss_is_error_ );

  std::string tmp;
  if ( !d->all_accessed( tmp ) ) // proceed only if there are unaccessed items left
  {
    // Fetch the target pointer here. We cannot do it above, since
    // Network::set_status() may modify the root compound if the number
    // of threads changes. HEP, 2008-10-20
    Node* target = local_nodes_.get_node_by_gid( gid );
    assert( target != 0 );

    for ( size_t t = 0; t < target->num_thread_siblings_(); ++t )
    {
      // Root container for per-thread subnets. We must prevent clearing of access
      // flags before each compound's properties are set by passing false as last arg
      // we iterate over all threads
      assert( target->get_thread_sibling_( t ) != 0 );
      set_status_single_node_( *( target->get_thread_sibling_( t ) ), d, false );
    }
  }
}

void
Network::set_status_single_node_( Node& target, const DictionaryDatum& d, bool clear_flags )
{
  // proxies have no properties
  if ( !target.is_proxy() )
  {
    if ( clear_flags )
      d->clear_access_flags();
    target.set_status_base( d );
    std::string missed;
    if ( !d->all_accessed( missed ) )
    {
      if ( dict_miss_is_error() )
        throw UnaccessedDictionaryEntry( missed );
      else
        LOG( M_WARNING, "Network::set_status", ( "Unread dictionary entries: " + missed ).c_str() );
    }
  }
}

DictionaryDatum
Network::get_status( index idx )
{
  Node* target = get_node( idx );
  assert( target != 0 );
  assert( initialized_ );

  DictionaryDatum d = target->get_status_base();

  if ( target == root_ )
  {
    // former scheduler_.get_status( d ) start
    kernel().get_status( d );


    def< long >( d, "send_buffer_size", Communicator::get_send_buffer_size() );
    def< long >( d, "receive_buffer_size", Communicator::get_recv_buffer_size() );
    // former scheduler_.get_status( d ) end

    connection_manager_.get_status( d );

    ( *d )[ "network_size" ] = size();
    ( *d )[ "dict_miss_is_error" ] = dict_miss_is_error_;

    std::map< long, size_t > sna_cts = local_nodes_.get_step_ctr();
    DictionaryDatum cdict( new Dictionary );
    for ( std::map< long, size_t >::const_iterator cit = sna_cts.begin(); cit != sna_cts.end();
          ++cit )
    {
      std::stringstream s;
      s << cit->first;
      ( *cdict )[ s.str() ] = cit->second;
    }
  }
  return d;
}


index
Network::copy_model( index old_id, std::string new_name )
{
  // we can assert here, as nestmodule checks this for us
  assert( !modeldict_->known( new_name ) );

  Model* new_model = get_model( old_id )->clone( new_name );
  models_.push_back( new_model );
  int new_id = models_.size() - 1;
  modeldict_->insert( new_name, new_id );
  int proxy_model_id = get_model_id( "proxynode" );
  assert( proxy_model_id > 0 );
  Model* proxy_model = models_[ proxy_model_id ];
  assert( proxy_model != 0 );
  for ( index t = 0; t < kernel().vp_manager.get_num_threads(); ++t )
  {
    Node* newnode = proxy_model->allocate( t );
    newnode->set_model_id( new_id );
    proxy_nodes_[ t ].push_back( newnode );
  }
  return new_id;
}

void
Network::register_basis_model( Model& m, bool private_model )
{
  std::string name = m.get_name();

  if ( !private_model && modeldict_->known( name ) )
  {
    delete &m;
    throw NamingConflict("A model called '" + name + "' already exists. "
        "Please choose a different name!");
  }
  pristine_models_.push_back( std::pair< Model*, bool >( &m, private_model ) );
}


index
Network::register_model( Model& m, bool private_model )
{
  std::string name = m.get_name();

  if ( !private_model && modeldict_->known( name ) )
  {
    delete &m;
    throw NamingConflict("A model called '" + name + "' already exists.\n"
        "Please choose a different name!");
  }

  const index id = models_.size();
  m.set_model_id( id );
  m.set_type_id( id );

  pristine_models_.push_back( std::pair< Model*, bool >( &m, private_model ) );
  models_.push_back( m.clone( name ) );
  int proxy_model_id = get_model_id( "proxynode" );
  assert( proxy_model_id > 0 );
  Model* proxy_model = models_[ proxy_model_id ];
  assert( proxy_model != 0 );

  for ( index t = 0; t < kernel().vp_manager.get_num_threads(); ++t )
  {
    Node* newnode = proxy_model->allocate( t );
    newnode->set_model_id( id );
    proxy_nodes_[ t ].push_back( newnode );
  }

  if ( !private_model )
    modeldict_->insert( name, id );

  return id;
}


/**
 * This function is not thread save and has to be called inside a omp critical
 * region, e.g. sli_neuron.
 */
int
Network::execute_sli_protected( DictionaryDatum state, Name cmd )
{
  SLIInterpreter& i = interpreter_;

  i.DStack->push( state ); // push state dictionary as top namespace
  size_t exitlevel = i.EStack.load();
  i.EStack.push( new NameDatum( cmd ) );
  int result = i.execute_( exitlevel );
  i.DStack->pop(); // pop neuron's namespace

  if ( state->known( "error" ) )
  {
    assert( state->known( names::global_id ) );
    index g_id = ( *state )[ names::global_id ];
    std::string model = getValue< std::string >( ( *state )[ names::model ] );
    std::string msg = String::compose( "Error in %1 with global id %2.", model, g_id );

    LOG( M_ERROR, cmd.toString().c_str(), msg.c_str() );
    LOG( M_ERROR, "execute_sli_protected", "Terminating." );

    kernel().simulation_manager.terminate();
  }

  return result;
}

#ifdef HAVE_MUSIC
void
Network::register_music_in_port( std::string portname )
{
  std::map< std::string, MusicPortData >::iterator it;
  it = music_in_portlist_.find( portname );
  if ( it == music_in_portlist_.end() )
    music_in_portlist_[ portname ] = MusicPortData( 1, 0.0, -1 );
  else
    music_in_portlist_[ portname ].n_input_proxies++;
}

void
Network::unregister_music_in_port( std::string portname )
{
  std::map< std::string, MusicPortData >::iterator it;
  it = music_in_portlist_.find( portname );
  if ( it == music_in_portlist_.end() )
    throw MUSICPortUnknown( portname );
  else
    music_in_portlist_[ portname ].n_input_proxies--;

  if ( music_in_portlist_[ portname ].n_input_proxies == 0 )
    music_in_portlist_.erase( it );
}

void
Network::register_music_event_in_proxy( std::string portname, int channel, nest::Node* mp )
{
  std::map< std::string, MusicEventHandler >::iterator it;
  it = music_in_portmap_.find( portname );
  if ( it == music_in_portmap_.end() )
  {
    MusicEventHandler tmp( portname,
      music_in_portlist_[ portname ].acceptable_latency,
      music_in_portlist_[ portname ].max_buffered );
    tmp.register_channel( channel, mp );
    music_in_portmap_[ portname ] = tmp;
  }
  else
    it->second.register_channel( channel, mp );
}

void
Network::set_music_in_port_acceptable_latency( std::string portname, double latency )
{
  std::map< std::string, MusicPortData >::iterator it;
  it = music_in_portlist_.find( portname );
  if ( it == music_in_portlist_.end() )
    throw MUSICPortUnknown( portname );
  else
    music_in_portlist_[ portname ].acceptable_latency = latency;
}

void
Network::set_music_in_port_max_buffered( std::string portname, int_t maxbuffered )
{
  std::map< std::string, MusicPortData >::iterator it;
  it = music_in_portlist_.find( portname );
  if ( it == music_in_portlist_.end() )
    throw MUSICPortUnknown( portname );
  else
    music_in_portlist_[ portname ].max_buffered = maxbuffered;
}

void
Network::publish_music_in_ports_()
{
  std::map< std::string, MusicEventHandler >::iterator it;
  for ( it = music_in_portmap_.begin(); it != music_in_portmap_.end(); ++it )
    it->second.publish_port();
}

void
Network::update_music_event_handlers_( Time const& origin, const long_t from, const long_t to )
{
  std::map< std::string, MusicEventHandler >::iterator it;
  for ( it = music_in_portmap_.begin(); it != music_in_portmap_.end(); ++it )
    it->second.update( origin, from, to );
}
#endif

/****** Previously Scheduler functions ***********/

//!< This function is called only if the thread data structures are properly set up.
void
nest::Network::finalize_nodes()
{
#ifdef _OPENMP
  LOG( M_INFO, "Network::finalize_nodes()", " using OpenMP." );
// parallel section begins
#pragma omp parallel
  {
    index t = kernel().vp_manager.get_thread_id();
#else
  for ( index t = 0; t < kernel().vp_manager.get_num_threads(); ++t )
  {
#endif
    for ( size_t idx = 0; idx < local_nodes_.size(); ++idx )
    {
      Node* node = local_nodes_.get_node_by_index( idx );
      if ( node != 0 )
      {
        if ( node->num_thread_siblings_() > 0 )
          node->get_thread_sibling_( t )->finalize();
        else
        {
          if ( static_cast< index >( node->get_thread() ) == t )
            node->finalize();
        }
      }
    }
  }
}

void
nest::Network::prepare_nodes()
{
  assert( initialized_ );

  kernel().event_delivery_manager.init_moduli_();

  LOG( M_INFO, "Network::prepare_nodes", "Please wait. Preparing elements." );

  /* We initialize the buffers of each node and calibrate it. */

  size_t num_active_nodes = 0; // counts nodes that will be updated

  std::vector< lockPTR< WrappedThreadException > > exceptions_raised(
    kernel().vp_manager.get_num_threads() );

#ifdef _OPENMP
#pragma omp parallel reduction( + : num_active_nodes )
  {
    size_t t = kernel().vp_manager.get_thread_id();
#else
  for ( index t = 0; t < kernel().vp_manager.get_num_threads(); ++t )
  {
#endif

    // We prepare nodes in a parallel region. Therefore, we need to catch exceptions
    // here and then handle them after the parallel region.
    try
    {
      for ( std::vector< Node* >::iterator it = nodes_vec_[ t ].begin();
            it != nodes_vec_[ t ].end();
            ++it )
      {
        prepare_node_( *it );
        if ( not( *it )->is_frozen() )
          ++num_active_nodes;
      }
    }
    catch ( std::exception& e )
    {
      // so throw the exception after parallel region
      exceptions_raised.at( t ) =
        lockPTR< WrappedThreadException >( new WrappedThreadException( e ) );
    }

  } // end of parallel section / end of for threads

  // check if any exceptions have been raised
  for ( index thr = 0; thr < kernel().vp_manager.get_num_threads(); ++thr )
    if ( exceptions_raised.at( thr ).valid() )
      throw WrappedThreadException( *( exceptions_raised.at( thr ) ) );

  LOG( M_INFO,
    "Network::prepare_nodes",
    String::compose(
         "Simulating %1 local node%2.", num_active_nodes, num_active_nodes == 1 ? "" : "s" ) );
}

void
nest::Network::update_nodes_vec_()
{
  // Check if the network size changed, in order to not enter
  // the critical region if it is not necessary. Note that this
  // test also covers that case that nodes have been deleted
  // by reset.
  if ( size() == nodes_vec_network_size_ )
    return;

#ifdef _OPENMP
#pragma omp critical( update_nodes_vec )
  {
// This code may be called from a thread-parallel context, when it is
// invoked by TargetIdentifierIndex::set_target() during parallel
// wiring. Nested OpenMP parallelism is problematic, therefore, we
// enforce single threading here. This should be unproblematic wrt
// performance, because the nodes_vec_ is rebuilt only once after
// changes in network size.
#endif

    // Check again, if the network size changed, since a previous thread
    // can have updated nodes_vec_ before.
    if ( size() != nodes_vec_network_size_ )
    {

      /* We clear the existing nodes_vec_ and then rebuild it. */
      assert( nodes_vec_.size() == kernel().vp_manager.get_num_threads() );

      for ( index t = 0; t < kernel().vp_manager.get_num_threads(); ++t )
      {
        nodes_vec_[ t ].clear();

        // Loops below run from index 1, because index 0 is always the root network,
        // which is never updated.
        size_t num_thread_local_nodes = 0;
        for ( size_t idx = 1; idx < local_nodes_.size(); ++idx )
        {
          Node* node = local_nodes_.get_node_by_index( idx );
          if ( !node->is_subnet() && ( static_cast< index >( node->get_thread() ) == t
                                       || node->num_thread_siblings_() > 0 ) )
            num_thread_local_nodes++;
        }
        nodes_vec_[ t ].reserve( num_thread_local_nodes );

        for ( size_t idx = 1; idx < local_nodes_.size(); ++idx )
        {
          Node* node = local_nodes_.get_node_by_index( idx );

          // Subnets are never updated and therefore not included.
          if ( node->is_subnet() )
            continue;

          // If a node has thread siblings, it is a sibling container, and we
          // need to add the replica for the current thread. Otherwise, we have
          // a normal node, which is added only on the thread it belongs to.
          if ( node->num_thread_siblings_() > 0 )
          {
            node->get_thread_sibling_( t )->set_thread_lid( nodes_vec_[ t ].size() );
            nodes_vec_[ t ].push_back( node->get_thread_sibling_( t ) );
          }
          else if ( static_cast< index >( node->get_thread() ) == t )
          {
            // these nodes cannot be subnets
            node->set_thread_lid( nodes_vec_[ t ].size() );
            nodes_vec_[ t ].push_back( node );
          }
        }
      } // end of for threads

      nodes_vec_network_size_ = size();
    }
#ifdef _OPENMP
  } // end of omp critical region
#endif
}

// inline
bool
Network::is_local_node( Node* n ) const
{
  return kernel().vp_manager.is_local_vp( n->get_vp() );
}

} // end of namespace
