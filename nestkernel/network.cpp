/*
 *  Network.cpp
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
#include "network_impl.h"
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

#include "nest_timemodifier.h"
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
  , dict_miss_is_error_( true )
  , model_defaults_modified_( false )
  , initialized_( false ) // scheduler stuff
  , simulating_( false )
  , n_rec_procs_( 0 )
  , n_sim_procs_( 0 )
  , clock_( Time::tic( 0L ) )
  , slice_( 0L )
  , to_do_( 0L )
  , to_do_total_( 0L )
  , from_step_( 0L )
  , to_step_( 0L ) // consistent with to_do_ == 0
  , terminate_( false )
  , off_grid_spiking_( false )
  , print_time_( false )
  , min_delay_( 1 )
  , max_delay_( 1 )
  , rng_()
  , comm_marker_( 0 )
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

  model = new GenericModel< proxynode >( "proxynode" );
  register_basis_model( *model, true );
  model->set_type_id( 2 );

  synapsedict_ = new Dictionary();
  interpreter_.def( "synapsedict", new DictionaryDatum( synapsedict_ ) );
  connection_manager_.init( synapsedict_ );

  connruledict_ = new Dictionary();
  interpreter_.def( "connruledict", new DictionaryDatum( connruledict_ ) );

  init_();
}

Network::~Network()
{
  clear_models_( true ); // mark call from destructor

  // Now we can delete the clean model prototypes
  vector< std::pair< Model*, bool > >::iterator i;
  for ( i = pristine_models_.begin(); i != pristine_models_.end(); ++i )
    if ( ( *i ).first != 0 )
      delete ( *i ).first;

  // clear the buffers
  local_grid_spikes_.clear();
  global_grid_spikes_.clear();
  local_offgrid_spikes_.clear();
  global_offgrid_spikes_.clear();

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
  node_model_ids_.add_range( 0, 0, 0 );

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

  simulated_ = false;

  // The following line is executed by all processes, no need to communicate
  // this change in delays.
  min_delay_ = max_delay_ = 1;

  n_sim_procs_ = Communicator::get_num_processes() - n_rec_procs_;

  create_rngs_( true ); // flag that this is a call from the ctr
  create_grng_( true ); // flag that this is a call from the ctr

  initialized_ = true;
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

  clear_models_();

  // We free all Node memory and set the number of threads.
  vector< std::pair< Model*, bool > >::iterator m;
  for ( m = pristine_models_.begin(); m != pristine_models_.end(); ++m )
  {
    // delete all nodes, because cloning the model may have created instances.
    ( *m ).first->clear();
    ( *m ).first->set_threads();
  }

  // former scheduler.reset()
  // Reset TICS_PER_MS, MS_PER_TICS and TICS_PER_STEP to the compiled in default values.
  // See ticket #217 for details.
  nest::TimeModifier::reset_to_defaults();

  clock_.set_to_zero(); // ensures consistent state
  to_do_ = 0;
  slice_ = 0;
  from_step_ = 0;
  to_step_ = 0; // consistent with to_do_ = 0

  // clear the buffers
  local_grid_spikes_.clear();
  global_grid_spikes_.clear();
  local_offgrid_spikes_.clear();
  global_offgrid_spikes_.clear();

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
  set_num_rec_processes( 0, true );
  dict_miss_is_error_ = true;

  reset();
}

void
Network::reset_network()
{
  if ( not get_simulated() )
    return; // nothing to do

  kernel().node_manager.reinit_nodes();

  // clear global spike buffers
  clear_pending_spikes();

  // ConnectionManager doesn't support resetting dynamic synapses yet
  LOG( M_WARNING,
    "ResetNetwork",
    "Synapses with internal dynamics (facilitation, STDP) are not reset.\n"
    "This will be implemented in a future version of NEST." );
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

bool
Network::model_in_use( index i )
{
  return node_model_ids_.model_in_use( i );
}

void
Network::simulate( Time const& t )
{
  assert( initialized_ );

  t_real_ = 0;
  t_slice_begin_ = timeval();
  t_slice_end_ = timeval();

  if ( t == Time::ms( 0.0 ) )
    return;

  if ( t < Time::step( 1 ) )
  {
    LOG( M_ERROR,
      "Network::simulate",
      String::compose(
           "Simulation time must be >= %1 ms (one time step).", Time::get_resolution().get_ms() ) );
    throw KernelException();
  }

  if ( t.is_finite() )
  {
    Time time1 = clock_ + t;
    if ( !time1.is_finite() )
    {
      std::string msg = String::compose(
        "A clock overflow will occur after %1 of %2 ms. Please reset network "
        "clock first!",
        ( Time::max() - clock_ ).get_ms(),
        t.get_ms() );
      LOG( M_ERROR, "Network::simulate", msg );
      throw KernelException();
    }
  }
  else
  {
    std::string msg = String::compose(
      "The requested simulation time exceeds the largest time NEST can handle "
      "(T_max = %1 ms). Please use a shorter time!",
      Time::max().get_ms() );
    LOG( M_ERROR, "Network::simulate", msg );
    throw KernelException();
  }

  to_do_ += t.get_steps();
  to_do_total_ = to_do_;

  prepare_simulation();

  // from_step_ is not touched here.  If we are at the beginning
  // of a simulation, it has been reset properly elsewhere.  If
  // a simulation was ended and is now continued, from_step_ will
  // have the proper value.  to_step_ is set as in advance_time().

  delay end_sim = from_step_ + to_do_;
  if ( min_delay_ < end_sim )
    to_step_ = min_delay_; // update to end of time slice
  else
    to_step_ = end_sim; // update to end of simulation time

  // Warn about possible inconsistencies, see #504.
  // This test cannot come any earlier, because we first need to compute min_delay_
  // above.
  if ( t.get_steps() % min_delay_ != 0 )
    LOG( M_WARNING,
      "Network::simulate",
      "The requested simulation time is not an integer multiple of the minimal delay in the "
      "network. "
      "This may result in inconsistent results under the following conditions: (i) A network "
      "contains "
      "more than one source of randomness, e.g., two different poisson_generators, and (ii) "
      "Simulate "
      "is called repeatedly with simulation times that are not multiples of the minimal delay." );

  resume();

  finalize_simulation();
}

void
Network::resume()
{
  assert( initialized_ );

  terminate_ = false;

  if ( to_do_ == 0 )
    return;

  if ( print_time_ )
  {
    std::cout << std::endl;
    print_progress_();
  }

  simulating_ = true;
  simulated_ = true;

#ifndef _OPENMP
  if ( n_threads_ > 1 )
  {
    LOG( M_ERROR, "Network::resume", "No multithreading available, using single threading" );
  }
#endif

  update();

  simulating_ = false;

  if ( print_time_ )
    std::cout << std::endl;

  Communicator::synchronize();

  if ( terminate_ )
  {
    LOG( M_ERROR, "Network::resume", "Exiting on error or user signal." );
    LOG( M_ERROR, "Network::resume", "Network: Use 'ResumeSimulation' to resume." );

    if ( SLIsignalflag != 0 )
    {
      SystemSignal signal( SLIsignalflag );
      SLIsignalflag = 0;
      throw signal;
    }
    else
      throw SimulationError();
  }

  LOG( M_INFO, "Network::resume", "Simulation finished." );
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
Network::set_status( index gid, const DictionaryDatum& d )
{
  // we first handle normal nodes, except the root (GID 0)
  if ( gid > 0 )
  {
    kernel().node_manager.set_status(gid, d);
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
       dictionary contains unknown entries. Thus, kernel().node_manager.set_status_single_node
       will not throw an exception
     - since all items in the root node are of type Compound, all read the same
       properties and we can leave the access flags set
   */
  d->clear_access_flags();

  // former scheduler_.set_status( d ); start
  // careful, this may invalidate all node pointers!
  assert( initialized_ );
  kernel().set_status( d );

  // Create an instance of time converter here to capture the current
  // representation of time objects: TICS_PER_MS and TICS_PER_STEP
  // will be stored in time_converter.
  // This object can then be used to convert times in steps
  // (e.g. Connection::delay_) or tics to the new representation.
  // We pass this object to ConnectionManager::calibrate to update
  // all time objects in the connection system to the new representation.
  // MH 08-04-14
  TimeConverter time_converter;

  double_t time;
  if ( updateValue< double_t >( d, "time", time ) )
  {
    if ( time != 0.0 )
      throw BadProperty( "The simulation time can only be set to 0.0." );

    if ( clock_ > TimeZero )
    {
      // reset only if time has passed
      LOG( M_WARNING,
        "Network::set_status",
        "Simulation time reset to t=0.0. Resetting the simulation time is not "
        "fully supported in NEST at present. Some spikes may be lost, and "
        "stimulating devices may behave unexpectedly. PLEASE REVIEW YOUR "
        "SIMULATION OUTPUT CAREFULLY!" );

      clock_ = Time::step( 0 );
      from_step_ = 0;
      slice_ = 0;
      configure_spike_buffers_(); // clear all old spikes
    }
  }

  updateValue< bool >( d, "print_time", print_time_ );

  // have those two for later asking, whether threads have changed:
  long n_threads;
  bool n_threads_updated = updateValue< long >( d, "local_num_threads", n_threads );

  // tics_per_ms and resolution must come after local_num_thread / total_num_threads
  // because they might reset the network and the time representation
  nest::double_t tics_per_ms;
  bool tics_per_ms_updated = updateValue< nest::double_t >( d, "tics_per_ms", tics_per_ms );
  double_t resd;
  bool res_updated = updateValue< double_t >( d, "resolution", resd );

  if ( tics_per_ms_updated || res_updated )
  {
    if ( kernel().node_manager.size() > 1 ) // root always exists
    {
      LOG( M_ERROR,
        "Network::set_status",
        "Cannot change time representation after nodes have been created. Please call ResetKernel "
        "first." );
      throw KernelException();
    }
    else if ( get_simulated() ) // someone may have simulated empty network
    {
      LOG( M_ERROR,
        "Network::set_status",
        "Cannot change time representation after the network has been simulated. Please call "
        "ResetKernel first." );
      throw KernelException();
    }
    else if ( connection_manager_.get_num_connections() != 0 )
    {
      LOG( M_ERROR,
        "Network::set_status",
        "Cannot change time representation after connections have been created. Please call "
        "ResetKernel first." );
      throw KernelException();
    }
    else if ( res_updated
      && tics_per_ms_updated ) // only allow TICS_PER_MS to be changed together with resolution
    {
      if ( resd < 1.0 / tics_per_ms )
      {
        LOG( M_ERROR,
          "Network::set_status",
          "Resolution must be greater than or equal to one tic. Value unchanged." );
        throw KernelException();
      }
      else
      {
        nest::TimeModifier::set_time_representation( tics_per_ms, resd );
        clock_.calibrate(); // adjust to new resolution
        connection_manager_.calibrate(
          time_converter ); // adjust delays in the connection system to new resolution
        LOG( M_INFO, "Network::set_status", "tics per ms and resolution changed." );
      }
    }
    else if ( res_updated ) // only resolution changed
    {
      if ( resd < Time::get_ms_per_tic() )
      {
        LOG( M_ERROR,
          "Network::set_status",
          "Resolution must be greater than or equal to one tic. Value unchanged." );
        throw KernelException();
      }
      else
      {
        Time::set_resolution( resd );
        clock_.calibrate(); // adjust to new resolution
        connection_manager_.calibrate(
          time_converter ); // adjust delays in the connection system to new resolution
        LOG( M_INFO, "Network::set_status", "Temporal resolution changed." );
      }
    }
    else
    {
      LOG( M_ERROR,
        "Network::set_status",
        "change of tics_per_step requires simultaneous specification of resolution." );
      throw KernelException();
    }
  }

  updateValue< bool >( d, "off_grid_spiking", off_grid_spiking_ );

  // set RNGs --- MUST come after n_threads_ is updated
  if ( d->known( "rngs" ) )
  {
    // this array contains pre-seeded RNGs, so they can be used
    // directly, no seeding required
    ArrayDatum* ad = dynamic_cast< ArrayDatum* >( ( *d )[ "rngs" ].datum() );
    if ( ad == 0 )
      throw BadProperty();

    // n_threads_ is the new value after a change of the number of
    // threads
    if ( ad->size() != ( size_t )( kernel().vp_manager.get_num_virtual_processes() ) )
    {
      LOG( M_ERROR,
        "Network::set_status",
        "Number of RNGs must equal number of virtual processes (threads*processes). RNGs "
        "unchanged." );
      throw DimensionMismatch(
        ( size_t )( kernel().vp_manager.get_num_virtual_processes() ), ad->size() );
    }

    // delete old generators, insert new generators this code is
    // robust under change of thread number in this call to
    // set_status, as long as it comes AFTER n_threads_ has been
    // upated
    rng_.clear();
    for ( index i = 0; i < ad->size(); ++i )
      if ( kernel().vp_manager.is_local_vp( i ) )
        rng_.push_back(
          getValue< librandom::RngDatum >( ( *ad )[ kernel().vp_manager.suggest_vp( i ) ] ) );
  }
  else if ( n_threads_updated && kernel().node_manager.size() == 0 )
  {
    LOG( M_WARNING, "Network::set_status", "Equipping threads with new default RNGs" );
    create_rngs_();
  }

  if ( d->known( "rng_seeds" ) )
  {
    ArrayDatum* ad = dynamic_cast< ArrayDatum* >( ( *d )[ "rng_seeds" ].datum() );
    if ( ad == 0 )
      throw BadProperty();

    if ( ad->size() != ( size_t )( kernel().vp_manager.get_num_virtual_processes() ) )
    {
      LOG( M_ERROR,
        "Network::set_status",
        "Number of seeds must equal number of virtual processes (threads*processes). RNGs "
        "unchanged." );
      throw DimensionMismatch(
        ( size_t )( kernel().vp_manager.get_num_virtual_processes() ), ad->size() );
    }

    // check if seeds are unique
    std::set< ulong_t > seedset;
    for ( index i = 0; i < ad->size(); ++i )
    {
      long s = ( *ad )[ i ]; // SLI has no ulong tokens
      if ( !seedset.insert( s ).second )
      {
        LOG( M_WARNING, "Network::set_status", "Seeds are not unique across threads!" );
        break;
      }
    }

    // now apply seeds, resets generators automatically
    for ( index i = 0; i < ad->size(); ++i )
    {
      long s = ( *ad )[ i ];

      if ( kernel().vp_manager.is_local_vp( i ) )
        rng_[ kernel().vp_manager.vp_to_thread( kernel().vp_manager.suggest_vp( i ) ) ]->seed( s );

      rng_seeds_[ i ] = s;
    }
  } // if rng_seeds

  // set GRNG
  if ( d->known( "grng" ) )
  {
    // pre-seeded grng that can be used directly, no seeding required
    updateValue< librandom::RngDatum >( d, "grng", grng_ );
  }
  else if ( n_threads_updated && kernel().node_manager.size() == 0 )
  {
    LOG( M_WARNING, "Network::set_status", "Equipping threads with new default GRNG" );
    create_grng_();
  }

  if ( d->known( "grng_seed" ) )
  {
    const long gseed = getValue< long >( d, "grng_seed" );

    // check if grng seed is unique with respect to rng seeds
    // if grng_seed and rng_seeds given in one SetStatus call
    std::set< ulong_t > seedset;
    seedset.insert( gseed );
    if ( d->known( "rng_seeds" ) )
    {
      ArrayDatum* ad_rngseeds = dynamic_cast< ArrayDatum* >( ( *d )[ "rng_seeds" ].datum() );
      if ( ad_rngseeds == 0 )
        throw BadProperty();
      for ( index i = 0; i < ad_rngseeds->size(); ++i )
      {
        const long vpseed = ( *ad_rngseeds )[ i ]; // SLI has no ulong tokens
        if ( !seedset.insert( vpseed ).second )
        {
          LOG( M_WARNING, "Network::set_status", "Seeds are not unique across threads!" );
          break;
        }
      }
    }
    // now apply seed, resets generator automatically
    grng_seed_ = gseed;
    grng_->seed( gseed );

  } // if grng_seed
  // former scheduler_.set_status( d ); end

  updateValue< bool >( d, "dict_miss_is_error", dict_miss_is_error_ );
}

DictionaryDatum
Network::get_status( index idx )
{
  Node* target = kernel().node_manager.get_node( idx );
  assert( target != 0 );
  assert( initialized_ );

  DictionaryDatum d = target->get_status_base();

  if ( target == kernel().node_manager.get_root() )
  {
    // former scheduler_.get_status( d ) start
    kernel().get_status( d );

    def< long >( d, "num_processes", Communicator::get_num_processes() );

    def< double_t >( d, "time", get_time().get_ms() );
    def< long >( d, "to_do", to_do_ );
    def< bool >( d, "print_time", print_time_ );

    def< double >( d, "tics_per_ms", Time::get_tics_per_ms() );
    def< double >( d, "resolution", Time::get_resolution().get_ms() );

    update_delay_extrema_();
    def< double >( d, "min_delay", Time( Time::step( min_delay_ ) ).get_ms() );
    def< double >( d, "max_delay", Time( Time::step( max_delay_ ) ).get_ms() );

    def< double >( d, "ms_per_tic", Time::get_ms_per_tic() );
    def< double >( d, "tics_per_ms", Time::get_tics_per_ms() );
    def< long >( d, "tics_per_step", Time::get_tics_per_step() );

    def< double >( d, "T_min", Time::min().get_ms() );
    def< double >( d, "T_max", Time::max().get_ms() );

    ( *d )[ "rng_seeds" ] = Token( rng_seeds_ );
    def< long >( d, "grng_seed", grng_seed_ );
    def< bool >( d, "off_grid_spiking", off_grid_spiking_ );
    def< long >( d, "send_buffer_size", Communicator::get_send_buffer_size() );
    def< long >( d, "receive_buffer_size", Communicator::get_recv_buffer_size() );
    // former scheduler_.get_status( d ) end

    connection_manager_.get_status( d );

    ( *d )[ "dict_miss_is_error" ] = dict_miss_is_error_;

  }
  return d;
}


// gid node thread syn delay weight
void
Network::connect( index sgid,
  Node* target,
  thread target_thread,
  index syn,
  double_t d,
  double_t w )
{
  Node* const source = kernel().node_manager.get_node( sgid, target_thread );

  // normal nodes and devices with proxies
  if ( target->has_proxies() )
  {
    connection_manager_.connect( *source, *target, sgid, target_thread, syn, d, w );
  }
  else if ( target->local_receiver() ) // normal devices
  {
    if ( source->is_proxy() )
      return;

    if ( ( source->get_thread() != target_thread ) && ( source->has_proxies() ) )
    {
      target_thread = source->get_thread();
      target = kernel().node_manager.get_node( target->get_gid(), target_thread );
    }

    connection_manager_.connect( *source, *target, sgid, target_thread, syn, d, w );
  }
  else // globally receiving devices iterate over all target threads
  {
    if ( !source->has_proxies() ) // we do not allow to connect a device to a global receiver at the
                                  // moment
      return;
    const thread n_threads = kernel().vp_manager.get_num_threads();
    for ( thread t = 0; t < n_threads; t++ )
    {
      target = kernel().node_manager.get_node( target->get_gid(), t );
      connection_manager_.connect( *source, *target, sgid, t, syn, d, w );
    }
  }
}

// gid node thread syn dict delay weight
void
Network::connect( index sgid,
  Node* target,
  thread target_thread,
  index syn,
  DictionaryDatum& params,
  double_t d,
  double_t w )
{
  Node* const source = kernel().node_manager.get_node( sgid, target_thread );

  // normal nodes and devices with proxies
  if ( target->has_proxies() )
  {
    connection_manager_.connect( *source, *target, sgid, target_thread, syn, params, d, w );
  }
  else if ( target->local_receiver() ) // normal devices
  {
    if ( source->is_proxy() )
      return;

    if ( ( source->get_thread() != target_thread ) && ( source->has_proxies() ) )
    {
      target_thread = source->get_thread();
      target = kernel().node_manager.get_node( target->get_gid(), target_thread );
    }

    connection_manager_.connect( *source, *target, sgid, target_thread, syn, params, d, w );
  }
  else // globally receiving devices iterate over all target threads
  {
    if ( !source->has_proxies() ) // we do not allow to connect a device to a global receiver at the
                                  // moment
      return;
    const thread n_threads = kernel().vp_manager.get_num_threads();
    for ( thread t = 0; t < n_threads; t++ )
    {
      target = kernel().node_manager.get_node( target->get_gid(), t );
      connection_manager_.connect( *source, *target, sgid, t, syn, params, d, w );
    }
  }
}

// gid gid dict
bool
Network::connect( index source_id, index target_id, DictionaryDatum& params, index syn )
{

  if ( !kernel().node_manager.is_local_gid( target_id ) )
    return false;

  Node* target_ptr = kernel().node_manager.get_node( target_id );

  // target_thread defaults to 0 for devices
  thread target_thread = target_ptr->get_thread();

  Node* source_ptr = kernel().node_manager.get_node( source_id, target_thread );

  // normal nodes and devices with proxies
  if ( target_ptr->has_proxies() )
  {
    connection_manager_.connect( *source_ptr, *target_ptr, source_id, target_thread, syn, params );
  }
  else if ( target_ptr->local_receiver() ) // normal devices
  {
    if ( source_ptr->is_proxy() )
      return false;

    if ( ( source_ptr->get_thread() != target_thread ) && ( source_ptr->has_proxies() ) )
    {
      target_thread = source_ptr->get_thread();
      target_ptr = kernel().node_manager.get_node( target_id, target_thread );
    }

    connection_manager_.connect( *source_ptr, *target_ptr, source_id, target_thread, syn, params );
  }
  else // globally receiving devices iterate over all target threads
  {
    if ( !source_ptr->has_proxies() ) // we do not allow to connect a device to a global receiver at
                                      // the moment
      return false;
    const thread n_threads = kernel().vp_manager.get_num_threads();
    for ( thread t = 0; t < n_threads; t++ )
    {
      target_ptr = kernel().node_manager.get_node( target_id, t );
      connection_manager_.connect( *source_ptr, *target_ptr, source_id, t, syn, params );
    }
  }

  // We did not exit prematurely due to proxies, so we have connected.
  return true;
}

// -----------------------------------------------------------------------------

void
Network::divergent_connect( index source_id,
  const TokenArray target_ids,
  const TokenArray weights,
  const TokenArray delays,
  index syn )
{
  bool complete_wd_lists = ( target_ids.size() == weights.size() && weights.size() != 0
    && weights.size() == delays.size() );
  bool short_wd_lists =
    ( target_ids.size() != weights.size() && weights.size() == 1 && delays.size() == 1 );
  bool no_wd_lists = ( weights.size() == 0 && delays.size() == 0 );

  // check if we have consistent lists for weights and delays
  if ( !( complete_wd_lists || short_wd_lists || no_wd_lists ) )
  {
    LOG( M_ERROR,
      "DivergentConnect",
      "If explicitly specified, weights and delays must be either doubles or lists of "
      "equal size. If given as lists, their size must be 1 or the same size as targets." );
    throw DimensionMismatch();
  }

  Node* source = kernel().node_manager.get_node( source_id );

  Subnet* source_comp = dynamic_cast< Subnet* >( source );
  if ( source_comp != 0 )
  {
    LOG( M_INFO, "DivergentConnect", "Source ID is a subnet; I will iterate it." );

    // collect all leaves in source subnet, then divergent-connect each leaf
    LocalLeafList local_sources( *source_comp );
    vector< Communicator::NodeAddressingData > global_sources;
    nest::Communicator::communicate( local_sources, global_sources );
    for ( vector< Communicator::NodeAddressingData >::iterator src = global_sources.begin();
          src != global_sources.end();
          ++src )
      divergent_connect( src->get_gid(), target_ids, weights, delays, syn );

    return;
  }

  // We retrieve pointers for all targets, this implicitly checks if they
  // exist and throws UnknownNode if not.
  std::vector< Node* > targets;
  targets.reserve( target_ids.size() );

  // only bother with local targets - is_local_gid is cheaper than kernel().node_manager.get_node()
  for ( index i = 0; i < target_ids.size(); ++i )
  {
    index gid = getValue< long >( target_ids[ i ] );
    if ( kernel().node_manager.is_local_gid( gid ) )
      targets.push_back( kernel().node_manager.get_node( gid ) );
  }

  for ( index i = 0; i < targets.size(); ++i )
  {
    thread target_thread = targets[ i ]->get_thread();

    if ( source->get_thread() != target_thread )
      source = kernel().node_manager.get_node( source_id, target_thread );

    if ( !targets[ i ]->has_proxies() && source->is_proxy() )
      continue;

    try
    {
      if ( complete_wd_lists )
        connection_manager_.connect( *source,
          *targets[ i ],
          source_id,
          target_thread,
          syn,
          delays.get( i ),
          weights.get( i ) );
      else if ( short_wd_lists )
        connection_manager_.connect( *source,
          *targets[ i ],
          source_id,
          target_thread,
          syn,
          delays.get( 0 ),
          weights.get( 0 ) );
      else
        connection_manager_.connect( *source, *targets[ i ], source_id, target_thread, syn );
    }
    catch ( IllegalConnection& e )
    {
      std::string msg = String::compose(
        "Target with ID %1 does not support the connection. "
        "The connection will be ignored.",
        targets[ i ]->get_gid() );
      if ( !e.message().empty() )
        msg += "\nDetails: " + e.message();
      LOG( M_WARNING, "DivergentConnect", msg.c_str() );
      continue;
    }
    catch ( UnknownReceptorType& e )
    {
      std::string msg = String::compose(
        "In Connection from global source ID %1 to target ID %2: "
        "Target does not support requested receptor type. "
        "The connection will be ignored",
        source->get_gid(),
        targets[ i ]->get_gid() );
      if ( !e.message().empty() )
        msg += "\nDetails: " + e.message();
      LOG( M_WARNING, "DivergentConnect", msg.c_str() );
      continue;
    }
    catch ( TypeMismatch& e )
    {
      std::string msg = String::compose(
        "In Connection from global source ID %1 to target ID %2: "
        "Expect source and weights of type double. "
        "The connection will be ignored",
        source->get_gid(),
        targets[ i ]->get_gid() );
      if ( !e.message().empty() )
        msg += "\nDetails: " + e.message();
      LOG( M_WARNING, "DivergentConnect", msg.c_str() );
      continue;
    }
  }
}

// -----------------------------------------------------------------------------


void
Network::divergent_connect( index source_id, DictionaryDatum pars, index syn )
{
  // We extract the parameters from the dictionary explicitly since getValue() for DoubleVectorDatum
  // copies the data into an array, from which the data must then be copied once more.
  DictionaryDatum par_i( new Dictionary() );
  Dictionary::iterator di_s, di_t;

  // To save time, we first create the parameter dictionary for connect(), then we copy
  // all keys from the original dictionary into the parameter dictionary.
  // We can the later use iterators to change the values inside the parameter dictionary,
  // rather than using the lookup operator.
  // We also do the parameter checking here so that we can later use unsafe operations.
  for ( di_s = ( *pars ).begin(); di_s != ( *pars ).end(); ++di_s )
  {
    par_i->insert( di_s->first, Token( new DoubleDatum() ) );
    DoubleVectorDatum const* tmp = dynamic_cast< DoubleVectorDatum* >( di_s->second.datum() );
    if ( tmp == 0 )
    {

      std::string msg = String::compose(
        "Parameter '%1' must be a DoubleVectorArray or numpy.array. ", di_s->first.toString() );
      LOG( M_DEBUG, "DivergentConnect", msg );
      LOG( M_DEBUG, "DivergentConnect", "Trying to convert, but this takes time." );

      IntVectorDatum const* tmpint = dynamic_cast< IntVectorDatum* >( di_s->second.datum() );
      if ( tmpint )
      {
        std::vector< double >* data =
          new std::vector< double >( ( *tmpint )->begin(), ( *tmpint )->end() );
        DoubleVectorDatum* dvd = new DoubleVectorDatum( data );
        di_s->second = dvd;
        continue;
      }
      ArrayDatum* ad = dynamic_cast< ArrayDatum* >( di_s->second.datum() );
      if ( ad )
      {
        std::vector< double >* data = new std::vector< double >;
        ad->toVector( *data );
        DoubleVectorDatum* dvd = new DoubleVectorDatum( data );
        di_s->second = dvd;
      }
      else
        throw TypeMismatch( DoubleVectorDatum().gettypename().toString() + " or "
            + ArrayDatum().gettypename().toString(),
          di_s->second.datum()->gettypename().toString() );
    }
  }

  const Token target_t = pars->lookup2( names::target );
  DoubleVectorDatum const* ptarget_ids = static_cast< DoubleVectorDatum* >( target_t.datum() );
  const std::vector< double >& target_ids( **ptarget_ids );

  const Token weight_t = pars->lookup2( names::weight );
  DoubleVectorDatum const* pweights = static_cast< DoubleVectorDatum* >( weight_t.datum() );
  const std::vector< double >& weights( **pweights );

  const Token delay_t = pars->lookup2( names::delay );
  DoubleVectorDatum const* pdelays = static_cast< DoubleVectorDatum* >( delay_t.datum() );
  const std::vector< double >& delays( **pdelays );


  bool complete_wd_lists =
    ( target_ids.size() == weights.size() && weights.size() == delays.size() );
  // check if we have consistent lists for weights and delays
  if ( !complete_wd_lists )
  {
    LOG(
      M_ERROR, "DivergentConnect", "All lists in the paramter dictionary must be of equal size." );
    throw DimensionMismatch();
  }

  Node* source = kernel().node_manager.get_node( source_id );

  Subnet* source_comp = dynamic_cast< Subnet* >( source );
  if ( source_comp != 0 )
  {
    LOG( M_INFO, "DivergentConnect", "Source ID is a subnet; I will iterate it." );

    // collect all leaves in source subnet, then divergent-connect each leaf
    LocalLeafList local_sources( *source_comp );
    vector< Communicator::NodeAddressingData > global_sources;
    nest::Communicator::communicate( local_sources, global_sources );
    for ( vector< Communicator::NodeAddressingData >::iterator src = global_sources.begin();
          src != global_sources.end();
          ++src )
      divergent_connect( src->get_gid(), pars, syn );

    return;
  }

  size_t n_targets = target_ids.size();
  for ( index i = 0; i < n_targets; ++i )
  {
    try
    {
      kernel().node_manager.get_node( target_ids[ i ] );
    }
    catch ( UnknownNode& e )
    {
      std::string msg = String::compose(
        "Target with ID %1 does not exist. "
        "The connection will be ignored.",
        target_ids[ i ] );
      if ( !e.message().empty() )
        msg += "\nDetails: " + e.message();
      LOG( M_WARNING, "DivergentConnect", msg.c_str() );
      continue;
    }

    // here we fill a parameter dictionary with the values of the current loop index.
    for ( di_s = ( *pars ).begin(), di_t = par_i->begin(); di_s != ( *pars ).end(); ++di_s, ++di_t )
    {
      DoubleVectorDatum const* tmp = static_cast< DoubleVectorDatum* >( di_s->second.datum() );
      const std::vector< double >& tmpvec = **tmp;
      DoubleDatum* dd = static_cast< DoubleDatum* >( di_t->second.datum() );
      ( *dd ) = tmpvec[ i ]; // We assign the double directly into the double datum.
    }

    try
    {
      connect( source_id, target_ids[ i ], par_i, syn );
    }
    catch ( UnexpectedEvent& e )
    {
      std::string msg = String::compose(
        "Target with ID %1 does not support the connection. "
        "The connection will be ignored.",
        target_ids[ i ] );
      if ( !e.message().empty() )
        msg += "\nDetails: " + e.message();
      LOG( M_WARNING, "DivergentConnect", msg.c_str() );
      continue;
    }
    catch ( IllegalConnection& e )
    {
      std::string msg = String::compose(
        "Target with ID %1 does not support the connection. "
        "The connection will be ignored.",
        target_ids[ i ] );
      if ( !e.message().empty() )
        msg += "\nDetails: " + e.message();
      LOG( M_WARNING, "DivergentConnect", msg.c_str() );
      continue;
    }
    catch ( UnknownReceptorType& e )
    {
      std::string msg = String::compose(
        "In Connection from global source ID %1 to target ID %2: "
        "Target does not support requested receptor type. "
        "The connection will be ignored",
        source_id,
        target_ids[ i ] );
      if ( !e.message().empty() )
        msg += "\nDetails: " + e.message();
      LOG( M_WARNING, "DivergentConnect", msg.c_str() );
      continue;
    }
  }
}


void
Network::random_divergent_connect( index source_id,
  const TokenArray target_ids,
  index n,
  const TokenArray weights,
  const TokenArray delays,
  bool allow_multapses,
  bool allow_autapses,
  index syn )
{
  Node* source = kernel().node_manager.get_node( source_id );

  // check if we have consistent lists for weights and delays
  if ( !( weights.size() == n || weights.size() == 0 ) && ( weights.size() == delays.size() ) )
  {
    LOG( M_ERROR, "RandomDivergentConnect", "weights and delays must be lists of size n." );
    throw DimensionMismatch();
  }

  Subnet* source_comp = dynamic_cast< Subnet* >( source );
  if ( source_comp != 0 )
  {
    LOG( M_INFO, "RandomDivergentConnect", "Source ID is a subnet; I will iterate it." );

    // collect all leaves in source subnet, then divergent-connect each leaf
    LocalLeafList local_sources( *source_comp );
    vector< Communicator::NodeAddressingData > global_sources;
    nest::Communicator::communicate( local_sources, global_sources );

    for ( vector< Communicator::NodeAddressingData >::iterator src = global_sources.begin();
          src != global_sources.end();
          ++src )
      random_divergent_connect(
        src->get_gid(), target_ids, n, weights, delays, allow_multapses, allow_autapses, syn );

    return;
  }

  librandom::RngPtr rng = get_grng();

  TokenArray chosen_targets;

  std::set< long > ch_ids; // ch_ids used for multapses identification

  long n_rnd = target_ids.size();

  for ( size_t j = 0; j < n; ++j )
  {
    long t_id;

    do
    {
      t_id = rng->ulrand( n_rnd );
    } while ( ( !allow_autapses && ( ( index ) target_ids.get( t_id ) ) == source_id )
      || ( !allow_multapses && ch_ids.find( t_id ) != ch_ids.end() ) );

    if ( !allow_multapses )
      ch_ids.insert( t_id );

    chosen_targets.push_back( target_ids.get( t_id ) );
  }

  divergent_connect( source_id, chosen_targets, weights, delays, syn );
}

// -----------------------------------------------------------------------------

void
Network::convergent_connect( const TokenArray source_ids,
  index target_id,
  const TokenArray weights,
  const TokenArray delays,
  index syn )
{
  bool complete_wd_lists = ( source_ids.size() == weights.size() && weights.size() != 0
    && weights.size() == delays.size() );
  bool short_wd_lists =
    ( source_ids.size() != weights.size() && weights.size() == 1 && delays.size() == 1 );
  bool no_wd_lists = ( weights.size() == 0 && delays.size() == 0 );

  // check if we have consistent lists for weights and delays
  if ( !( complete_wd_lists || short_wd_lists || no_wd_lists ) )
  {
    LOG( M_ERROR,
      "ConvergentConnect",
      "weights and delays must be either doubles or lists of equal size. "
      "If given as lists, their size must be 1 or the same size as sources." );
    throw DimensionMismatch();
  }

  if ( !kernel().node_manager.is_local_gid( target_id ) )
    return;

  Node* target = kernel().node_manager.get_node( target_id );

  Subnet* target_comp = dynamic_cast< Subnet* >( target );
  if ( target_comp != 0 )
  {
    LOG( M_INFO, "ConvergentConnect", "Target node is a subnet; I will iterate it." );

    // we only iterate over local leaves, as remote targets are ignored anyways
    LocalLeafList target_nodes( *target_comp );
    for ( LocalLeafList::iterator tgt = target_nodes.begin(); tgt != target_nodes.end(); ++tgt )
      convergent_connect( source_ids, ( *tgt )->get_gid(), weights, delays, syn );

    return;
  }

  for ( index i = 0; i < source_ids.size(); ++i )
  {
    index source_id = source_ids.get( i );
    Node* source = kernel().node_manager.get_node( getValue< long >( source_id ) );

    thread target_thread = target->get_thread();

    if ( !target->has_proxies() )
    {
      // target_thread = sources[i]->get_thread();
      target_thread = source->get_thread();

      // If target is on the wrong thread, we need to get the right one now.
      if ( target->get_thread() != target_thread )
        target = kernel().node_manager.get_node( target_id, target_thread );

      if ( source->is_proxy() )
        continue;
    }

    // The source node may still be on a wrong thread, so we need to get the right
    // one now. As get_node() is quite expensive, so we only call it if we need to
    // if (source->get_thread() != target_thread)
    //  source = get_node(sid, target_thread);

    try
    {
      if ( complete_wd_lists )
        connection_manager_.connect(
          *source, *target, source_id, target_thread, syn, delays.get( i ), weights.get( i ) );
      else if ( short_wd_lists )
        connection_manager_.connect(
          *source, *target, source_id, target_thread, syn, delays.get( 0 ), weights.get( 0 ) );
      else
        connection_manager_.connect( *source, *target, source_id, target_thread, syn );
    }
    catch ( IllegalConnection& e )
    {
      std::string msg = String::compose(
        "Target with ID %1 does not support the connection. "
        "The connection will be ignored.",
        target->get_gid() );
      if ( !e.message().empty() )
        msg += "\nDetails: " + e.message();
      LOG( M_WARNING, "ConvergentConnect", msg.c_str() );
      continue;
    }
    catch ( UnknownReceptorType& e )
    {
      std::string msg = String::compose(
        "In Connection from global source ID %1 to target ID %2: "
        "Target does not support requested receptor type. "
        "The connection will be ignored",
        source->get_gid(),
        target->get_gid() );
      if ( !e.message().empty() )
        msg += "\nDetails: " + e.message();
      LOG( M_WARNING, "ConvergentConnect", msg.c_str() );
      continue;
    }
    catch ( TypeMismatch& e )
    {
      std::string msg = String::compose(
        "In Connection from global source ID %1 to target ID %2: "
        "Expect source and weights of type double. "
        "The connection will be ignored",
        source->get_gid(),
        target->get_gid() );
      if ( !e.message().empty() )
        msg += "\nDetails: " + e.message();
      LOG( M_WARNING, "ConvergentConnect", msg.c_str() );
      continue;
    }
  }
}


/**
 * New and specialized variant of the convergent_connect()
 * function, which takes a vector<Node*> for sources and relies
 * on the fact that target is guaranteed to be on this thread.
 */
void
Network::convergent_connect( const std::vector< index >& source_ids,
  index target_id,
  const TokenArray& weights,
  const TokenArray& delays,
  index syn )
{
  bool complete_wd_lists = ( source_ids.size() == weights.size() && weights.size() != 0
    && weights.size() == delays.size() );
  bool short_wd_lists =
    ( source_ids.size() != weights.size() && weights.size() == 1 && delays.size() == 1 );

  // Check if we have consistent lists for weights and delays
  // already checked in previous RCC call

  Node* target = kernel().node_manager.get_node( target_id );
  for ( index i = 0; i < source_ids.size(); ++i )
  {
    Node* source = kernel().node_manager.get_node( source_ids[ i ] );
    thread target_thread = target->get_thread();

    if ( !target->has_proxies() )
    {
      target_thread = source->get_thread();

      // If target is on the wrong thread, we need to get the right one now.
      if ( target->get_thread() != target_thread )
        target = kernel().node_manager.get_node( target_id, target_thread );

      if ( source->is_proxy() )
        continue;
    }

    try
    {
      if ( complete_wd_lists )
        connection_manager_.connect( *source,
          *target,
          source_ids[ i ],
          target_thread,
          syn,
          delays.get( i ),
          weights.get( i ) );
      else if ( short_wd_lists )
        connection_manager_.connect( *source,
          *target,
          source_ids[ i ],
          target_thread,
          syn,
          delays.get( 0 ),
          weights.get( 0 ) );
      else
        connection_manager_.connect( *source, *target, source_ids[ i ], target_thread, syn );
    }
    catch ( IllegalConnection& e )
    {
      std::string msg = String::compose(
        "Target with ID %1 does not support the connection. "
        "The connection will be ignored.",
        target->get_gid() );
      if ( !e.message().empty() )
        msg += "\nDetails: " + e.message();
      LOG( M_WARNING, "ConvergentConnect", msg.c_str() );
      continue;
    }
    catch ( UnknownReceptorType& e )
    {
      std::string msg = String::compose(
        "In Connection from global source ID %1 to target ID %2: "
        "Target does not support requested receptor type. "
        "The connection will be ignored",
        source->get_gid(),
        target->get_gid() );
      if ( !e.message().empty() )
        msg += "\nDetails: " + e.message();
      LOG( M_WARNING, "ConvergentConnect", msg.c_str() );
      continue;
    }
    catch ( TypeMismatch& e )
    {
      std::string msg = String::compose(
        "In Connection from global source ID %1 to target ID %2: "
        "Expect source and weights of type double. "
        "The connection will be ignored",
        source->get_gid(),
        target->get_gid() );
      if ( !e.message().empty() )
        msg += "\nDetails: " + e.message();
      LOG( M_WARNING, "ConvergentConnect", msg.c_str() );
      continue;
    }
  }
}


void
Network::random_convergent_connect( const TokenArray source_ids,
  index target_id,
  index n,
  const TokenArray weights,
  const TokenArray delays,
  bool allow_multapses,
  bool allow_autapses,
  index syn )
{
  if ( !kernel().node_manager.is_local_gid( target_id ) )
    return;

  Node* target = kernel().node_manager.get_node( target_id );

  // check if we have consistent lists for weights and delays
  if ( !( weights.size() == n || weights.size() == 0 ) && ( weights.size() == delays.size() ) )
  {
    LOG( M_ERROR, "ConvergentConnect", "weights and delays must be lists of size n." );
    throw DimensionMismatch();
  }

  Subnet* target_comp = dynamic_cast< Subnet* >( target );
  if ( target_comp != 0 )
  {
    LOG( M_INFO, "RandomConvergentConnect", "Target ID is a subnet; I will iterate it." );

    // we only consider local leaves as targets,
    LocalLeafList target_nodes( *target_comp );
    for ( LocalLeafList::iterator tgt = target_nodes.begin(); tgt != target_nodes.end(); ++tgt )
      random_convergent_connect(
        source_ids, ( *tgt )->get_gid(), n, weights, delays, allow_multapses, allow_autapses, syn );

    return;
  }

  librandom::RngPtr rng = get_rng( target->get_thread() );
  TokenArray chosen_sources;

  std::set< long > ch_ids;

  long n_rnd = source_ids.size();

  for ( size_t j = 0; j < n; ++j )
  {
    long s_id;

    do
    {
      s_id = rng->ulrand( n_rnd );
    } while ( ( !allow_autapses && ( ( index ) source_ids[ s_id ] ) == target_id )
      || ( !allow_multapses && ch_ids.find( s_id ) != ch_ids.end() ) );

    if ( !allow_multapses )
      ch_ids.insert( s_id );

    chosen_sources.push_back( source_ids[ s_id ] );
  }

  convergent_connect( chosen_sources, target_id, weights, delays, syn );
}

// This function loops over all targets, with every thread taking
// care only of its own target nodes
void
Network::random_convergent_connect( TokenArray source_ids,
  TokenArray target_ids,
  TokenArray ns,
  TokenArray weights,
  TokenArray delays,
  bool allow_multapses,
  bool allow_autapses,
  index syn )
{
#ifndef _OPENMP
  // It only makes sense to call this function if we have openmp
  LOG( M_ERROR, "ConvergentConnect", "This function can only be called using OpenMP threading." );
  throw KernelException();
#else

  // Collect all nodes on this process and convert the TokenArray with
  // the sources to a std::vector<Node*>. This is needed, because
  // 1. We don't want to call get_node() within the loop for many
  //    neurons several times
  // 2. The function token_array::operator[]() is not thread-safe, so
  //    the threads will possibly access the same element at the same
  //    time, causing segfaults

  std::vector< index > vsource_ids( source_ids.size() );
  for ( index i = 0; i < source_ids.size(); ++i )
  {
    index sid = getValue< long >( source_ids.get( i ) );
    vsource_ids[ i ] = sid;
  }

  // Check if we have consistent lists for weights and delays
  if ( !( weights.size() == ns.size() || weights.size() == 0 )
    && ( weights.size() == delays.size() ) )
  {
    LOG( M_ERROR, "ConvergentConnect", "weights, delays and ns must be same size." );
    throw DimensionMismatch();
  }

  for ( size_t i = 0; i < ns.size(); ++i )
  {
    size_t n;
    // This throws std::bad_cast if the dynamic_cast goes
    // wrong. Throwing in a parallel section is not allowed. This
    // could be solved by only accepting IntVectorDatums for the ns.
    try
    {
      const IntegerDatum& nid = dynamic_cast< const IntegerDatum& >( *ns.get( i ) );
      n = nid.get();
    }
    catch ( const std::bad_cast& e )
    {
      LOG( M_ERROR, "ConvergentConnect", "ns must consist of integers only." );
      throw KernelException();
    }

    // Check if we have consistent lists for weights and delays part two.
    // The inner lists have to be equal to n or be zero.
    if ( weights.size() > 0 )
    {
      TokenArray ws = getValue< TokenArray >( weights.get( i ) );
      TokenArray ds = getValue< TokenArray >( delays.get( i ) );

      if ( !( ws.size() == n || ws.size() == 0 ) && ( ws.size() == ds.size() ) )
      {
        LOG( M_ERROR, "ConvergentConnect", "weights and delays must be lists of size n." );
        throw DimensionMismatch();
      }
    }
  }

#pragma omp parallel
  {
    int nrn_counter = 0;
    int tid = kernel().vp_manager.get_thread_id();

    librandom::RngPtr rng = get_rng( tid );

    for ( size_t i = 0; i < target_ids.size(); i++ )
    {
      index target_id = target_ids.get( i );

      // This is true for neurons on remote processes
      if ( !kernel().node_manager.is_local_gid( target_id ) )
        continue;

      Node* target = kernel().node_manager.get_node( target_id, tid );

      // Check, if target is on our thread
      if ( target->get_thread() != tid )
        continue;

      nrn_counter++;

      // extract number of connections for target i
      const IntegerDatum& nid = dynamic_cast< const IntegerDatum& >( *ns.get( i ) );
      const size_t n = nid.get();

      // extract weights and delays for all connections to target i
      TokenArray ws;
      TokenArray ds;
      if ( weights.size() > 0 )
      {
        ws = getValue< TokenArray >( weights.get( i ) );
        ds = getValue< TokenArray >( delays.get( i ) );
      }

      vector< index > chosen_source_ids( n );
      std::set< long > ch_ids;

      long n_rnd = vsource_ids.size();

      for ( size_t j = 0; j < n; ++j )
      {
        long s_id;

        do
        {
          s_id = rng->ulrand( n_rnd );
        } while ( ( !allow_autapses && ( ( index ) vsource_ids[ s_id ] ) == target_id )
          || ( !allow_multapses && ch_ids.find( s_id ) != ch_ids.end() ) );

        if ( !allow_multapses )
          ch_ids.insert( s_id );

        chosen_source_ids[ j ] = vsource_ids[ s_id ];
      }

      convergent_connect( chosen_source_ids, target_id, ws, ds, syn );

    } // of for all targets
  }   // of omp parallel
#endif
}

void
Network::connect( const GIDCollection& sources,
  const GIDCollection& targets,
  const DictionaryDatum& conn_spec,
  const DictionaryDatum& syn_spec )
{
  conn_spec->clear_access_flags();
  syn_spec->clear_access_flags();

  if ( !conn_spec->known( names::rule ) )
    throw BadProperty( "Connectivity spec must contain connectivity rule." );
  const std::string rule_name = ( *conn_spec )[ names::rule ];

  if ( !connruledict_->known( rule_name ) )
    throw BadProperty( "Unknown connectivty rule: " + rule_name );
  const long rule_id = ( *connruledict_ )[ rule_name ];

  ConnBuilder* cb =
    connbuilder_factories_.at( rule_id )->create( sources, targets, conn_spec, syn_spec );
  assert( cb != 0 );

  // at this point, all entries in conn_spec and syn_spec have been checked
  std::string missed;
  if ( !( conn_spec->all_accessed( missed ) && syn_spec->all_accessed( missed ) ) )
  {
    if ( dict_miss_is_error() )
      throw UnaccessedDictionaryEntry( missed );
    else
      LOG( M_WARNING, "Connect", ( "Unread dictionary entries: " + missed ).c_str() );
  }

  cb->connect();
  delete cb;
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

    terminate();
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

void
nest::Network::prepare_simulation()
{
  if ( to_do_ == 0 )
    return;

  // find shortest and longest delay across all MPI processes
  // this call sets the member variables
  update_delay_extrema_();

  // Check for synchronicity of global rngs over processes.
  // We need to do this ahead of any simulation in case random numbers
  // have been consumed on the SLI level.
  if ( Communicator::get_num_processes() > 1 )
  {
    if ( !Communicator::grng_synchrony( grng_->ulrand( 100000 ) ) )
    {
      LOG( M_ERROR,
        "Network::simulate",
        "Global Random Number Generators are not synchronized prior to simulation." );
      throw KernelException();
    }
  }

  // if at the beginning of a simulation, set up spike buffers
  if ( !simulated_ )
    configure_spike_buffers_();

  kernel().node_manager.ensure_valid_thread_local_ids();
  kernel().node_manager.prepare_nodes();

#ifdef HAVE_MUSIC
  // we have to do enter_runtime after prepre_nodes, since we use
  // calibrate to map the ports of MUSIC devices, which has to be done
  // before enter_runtime
  if ( !simulated_ ) // only enter the runtime mode once
  {
    publish_music_in_ports_();

    double tick = Time::get_resolution().get_ms() * min_delay_;
    std::string msg = String::compose( "Entering MUSIC runtime with tick = %1 ms", tick );
    LOG( M_INFO, "Network::resume", msg );
    Communicator::enter_runtime( tick );
  }
#endif
}

void
nest::Network::update()
{
#ifdef _OPENMP
  LOG( M_INFO, "Network::update", "Simulating using OpenMP." );
#endif

  std::vector< lockPTR< WrappedThreadException > > exceptions_raised(
    kernel().vp_manager.get_num_threads() );
// parallel section begins
#pragma omp parallel
  {
    std::vector< Node* >::iterator i;
    int t = kernel().vp_manager.get_thread_id();

    do
    {
      if ( print_time_ )
        gettimeofday( &t_slice_begin_, NULL );

      if ( from_step_ == 0 ) // deliver only at beginning of slice
      {
        deliver_events_( t );
#ifdef HAVE_MUSIC
// advance the time of music by one step (min_delay * h) must
// be done after deliver_events_() since it calls
// music_event_out_proxy::handle(), which hands the spikes over to
// MUSIC *before* MUSIC time is advanced

// wait until all threads are done -> synchronize
#pragma omp barrier
// the following block is executed by the master thread only
// the other threads are enforced to wait at the end of the block
#pragma omp master
        {
          // advance the time of music by one step (min_delay * h) must
          // be done after deliver_events_() since it calls
          // music_event_out_proxy::handle(), which hands the spikes over to
          // MUSIC *before* MUSIC time is advanced
          if ( slice_ > 0 )
            Communicator::advance_music_time( 1 );

          // the following could be made thread-safe
          update_music_event_handlers_( clock_, from_step_, to_step_ );
        }
// end of master section, all threads have to synchronize at this point
#pragma omp barrier
#endif
      }

      vector< vector< Node* > > nodes_vec = kernel().node_manager.get_nodes_vec();
      for ( i = nodes_vec[ t ].begin(); i != nodes_vec[ t ].end(); ++i )
      {
        // We update in a parallel region. Therefore, we need to catch exceptions
        // here and then handle them after the parallel region.
        try
        {
          if ( not( *i )->is_frozen() )
            ( *i )->update( clock_, from_step_, to_step_ );
        }
        catch ( std::exception& e )
        {
          // so throw the exception after parallel region
          exceptions_raised.at( t ) =
            lockPTR< WrappedThreadException >( new WrappedThreadException( e ) );
          terminate_ = true;
        }
      }

// parallel section ends, wait until all threads are done -> synchronize
#pragma omp barrier

// the following block is executed by the master thread only
// the other threads are enforced to wait at the end of the block
#pragma omp master
      {
        if ( to_step_ == min_delay_ ) // gather only at end of slice
          gather_events_();

        advance_time_();

        if ( SLIsignalflag != 0 )
        {
          LOG( M_INFO, "Network::update", "Simulation exiting on user signal." );
          terminate_ = true;
        }

        if ( print_time_ )
        {
          gettimeofday( &t_slice_end_, NULL );
          print_progress_();
        }
      }
// end of master section, all threads have to synchronize at this point
#pragma omp barrier

    } while ( ( to_do_ != 0 ) && ( !terminate_ ) );

  } // end of #pragma parallel omp
  // check if any exceptions have been raised
  for ( index thr = 0; thr < kernel().vp_manager.get_num_threads(); ++thr )
    if ( exceptions_raised.at( thr ).valid() )
      throw WrappedThreadException( *( exceptions_raised.at( thr ) ) );
}

void
nest::Network::finalize_simulation()
{
  if ( not simulated_ )
    return;

  // Check for synchronicity of global rngs over processes
  // TODO: This seems double up, there is such a test at end of simulate()
  if ( Communicator::get_num_processes() > 1 )
    if ( !Communicator::grng_synchrony( grng_->ulrand( 100000 ) ) )
    {
      LOG( M_ERROR,
        "Network::simulate",
        "Global Random Number Generators are not synchronized after simulation." );
      throw KernelException();
    }

  kernel().node_manager.finalize_nodes();
}

void
nest::Network::collocate_buffers_()
{
  // count number of spikes in registers
  int num_spikes = 0;
  int num_grid_spikes = 0;
  int num_offgrid_spikes = 0;

  std::vector< std::vector< std::vector< uint_t > > >::iterator i;
  std::vector< std::vector< uint_t > >::iterator j;
  for ( i = spike_register_.begin(); i != spike_register_.end(); ++i )
    for ( j = i->begin(); j != i->end(); ++j )
      num_grid_spikes += j->size();

  std::vector< std::vector< std::vector< OffGridSpike > > >::iterator it;
  std::vector< std::vector< OffGridSpike > >::iterator jt;
  for ( it = offgrid_spike_register_.begin(); it != offgrid_spike_register_.end(); ++it )
    for ( jt = it->begin(); jt != it->end(); ++jt )
      num_offgrid_spikes += jt->size();

  num_spikes = num_grid_spikes + num_offgrid_spikes;
  if ( !off_grid_spiking_ ) // on grid spiking
  {
    // make sure buffers are correctly sized
    if ( global_grid_spikes_.size()
      != static_cast< uint_t >( Communicator::get_recv_buffer_size() ) )
      global_grid_spikes_.resize( Communicator::get_recv_buffer_size(), 0 );

    if ( num_spikes + ( kernel().vp_manager.get_num_threads() * min_delay_ )
      > static_cast< uint_t >( Communicator::get_send_buffer_size() ) )
      local_grid_spikes_.resize(
        ( num_spikes + ( min_delay_ * kernel().vp_manager.get_num_threads() ) ), 0 );
    else if ( local_grid_spikes_.size()
      < static_cast< uint_t >( Communicator::get_send_buffer_size() ) )
      local_grid_spikes_.resize( Communicator::get_send_buffer_size(), 0 );

    // collocate the entries of spike_registers into local_grid_spikes__
    std::vector< uint_t >::iterator pos = local_grid_spikes_.begin();
    if ( num_offgrid_spikes == 0 )
      for ( i = spike_register_.begin(); i != spike_register_.end(); ++i )
        for ( j = i->begin(); j != i->end(); ++j )
        {
          pos = std::copy( j->begin(), j->end(), pos );
          *pos = comm_marker_;
          ++pos;
        }
    else
    {
      std::vector< OffGridSpike >::iterator n;
      it = offgrid_spike_register_.begin();
      for ( i = spike_register_.begin(); i != spike_register_.end(); ++i )
      {
        jt = it->begin();
        for ( j = i->begin(); j != i->end(); ++j )
        {
          pos = std::copy( j->begin(), j->end(), pos );
          for ( n = jt->begin(); n != jt->end(); ++n )
          {
            *pos = n->get_gid();
            ++pos;
          }
          *pos = comm_marker_;
          ++pos;
          ++jt;
        }
        ++it;
      }
      for ( it = offgrid_spike_register_.begin(); it != offgrid_spike_register_.end(); ++it )
        for ( jt = it->begin(); jt != it->end(); ++jt )
          jt->clear();
    }

    // remove old spikes from the spike_register_
    for ( i = spike_register_.begin(); i != spike_register_.end(); ++i )
      for ( j = i->begin(); j != i->end(); ++j )
        j->clear();
  }
  else // off_grid_spiking
  {
    // make sure buffers are correctly sized
    if ( global_offgrid_spikes_.size()
      != static_cast< uint_t >( Communicator::get_recv_buffer_size() ) )
      global_offgrid_spikes_.resize( Communicator::get_recv_buffer_size(), OffGridSpike( 0, 0.0 ) );

    if ( num_spikes + ( kernel().vp_manager.get_num_threads() * min_delay_ )
      > static_cast< uint_t >( Communicator::get_send_buffer_size() ) )
      local_offgrid_spikes_.resize(
        ( num_spikes + ( min_delay_ * kernel().vp_manager.get_num_threads() ) ),
        OffGridSpike( 0, 0.0 ) );
    else if ( local_offgrid_spikes_.size()
      < static_cast< uint_t >( Communicator::get_send_buffer_size() ) )
      local_offgrid_spikes_.resize( Communicator::get_send_buffer_size(), OffGridSpike( 0, 0.0 ) );

    // collocate the entries of spike_registers into local_offgrid_spikes__
    std::vector< OffGridSpike >::iterator pos = local_offgrid_spikes_.begin();
    if ( num_grid_spikes == 0 )
      for ( it = offgrid_spike_register_.begin(); it != offgrid_spike_register_.end(); ++it )
        for ( jt = it->begin(); jt != it->end(); ++jt )
        {
          pos = std::copy( jt->begin(), jt->end(), pos );
          pos->set_gid( comm_marker_ );
          ++pos;
        }
    else
    {
      std::vector< uint_t >::iterator n;
      i = spike_register_.begin();
      for ( it = offgrid_spike_register_.begin(); it != offgrid_spike_register_.end(); ++it )
      {
        j = i->begin();
        for ( jt = it->begin(); jt != it->end(); ++jt )
        {
          pos = std::copy( jt->begin(), jt->end(), pos );
          for ( n = j->begin(); n != j->end(); ++n )
          {
            *pos = OffGridSpike( *n, 0 );
            ++pos;
          }
          pos->set_gid( comm_marker_ );
          ++pos;
          ++j;
        }
        ++i;
      }
      for ( i = spike_register_.begin(); i != spike_register_.end(); ++i )
        for ( j = i->begin(); j != i->end(); ++j )
          j->clear();
    }

    // empty offgrid_spike_register_
    for ( it = offgrid_spike_register_.begin(); it != offgrid_spike_register_.end(); ++it )
      for ( jt = it->begin(); jt != it->end(); ++jt )
        jt->clear();
  }
}

void
nest::Network::deliver_events_( thread t )
{
  // deliver only at beginning of time slice
  if ( from_step_ > 0 )
    return;

  SpikeEvent se;

  std::vector< int > pos( displacements_ );

  if ( !off_grid_spiking_ ) // on_grid_spiking
  {
    // prepare Time objects for every possible time stamp within min_delay_
    std::vector< Time > prepared_timestamps( min_delay_ );
    for ( size_t lag = 0; lag < ( size_t ) min_delay_; lag++ )
    {
      prepared_timestamps[ lag ] = clock_ - Time::step( lag );
    }

    for ( size_t vp = 0; vp < ( size_t ) kernel().vp_manager.get_num_virtual_processes(); ++vp )
    {
      size_t pid = get_process_id( vp );
      int pos_pid = pos[ pid ];
      int lag = min_delay_ - 1;
      while ( lag >= 0 )
      {
        index nid = global_grid_spikes_[ pos_pid ];
        if ( nid != static_cast< index >( comm_marker_ ) )
        {
          // tell all local nodes about spikes on remote machines.
          se.set_stamp( prepared_timestamps[ lag ] );
          se.set_sender_gid( nid );
          connection_manager_.send( t, nid, se );
        }
        else
        {
          --lag;
        }
        ++pos_pid;
      }
      pos[ pid ] = pos_pid;
    }
  }
  else // off grid spiking
  {
    // prepare Time objects for every possible time stamp within min_delay_
    std::vector< Time > prepared_timestamps( min_delay_ );
    for ( size_t lag = 0; lag < ( size_t ) min_delay_; lag++ )
    {
      prepared_timestamps[ lag ] = clock_ - Time::step( lag );
    }

    for ( size_t vp = 0; vp < ( size_t ) kernel().vp_manager.get_num_virtual_processes(); ++vp )
    {
      size_t pid = get_process_id( vp );
      int pos_pid = pos[ pid ];
      int lag = min_delay_ - 1;
      while ( lag >= 0 )
      {
        index nid = global_offgrid_spikes_[ pos_pid ].get_gid();
        if ( nid != static_cast< index >( comm_marker_ ) )
        {
          // tell all local nodes about spikes on remote machines.
          se.set_stamp( prepared_timestamps[ lag ] );
          se.set_sender_gid( nid );
          se.set_offset( global_offgrid_spikes_[ pos_pid ].get_offset() );
          connection_manager_.send( t, nid, se );
        }
        else
        {
          --lag;
        }
        ++pos_pid;
      }
      pos[ pid ] = pos_pid;
    }
  }
}

void
nest::Network::gather_events_()
{
  collocate_buffers_();
  if ( off_grid_spiking_ )
    Communicator::communicate( local_offgrid_spikes_, global_offgrid_spikes_, displacements_ );
  else
    Communicator::communicate( local_grid_spikes_, global_grid_spikes_, displacements_ );
}

void
nest::Network::advance_time_()
{
  // time now advanced time by the duration of the previous step
  to_do_ -= to_step_ - from_step_;

  // advance clock, update modulos, slice counter only if slice completed
  if ( ( delay ) to_step_ == min_delay_ )
  {
    clock_ += Time::step( min_delay_ );
    ++slice_;
    compute_moduli_();
    from_step_ = 0;
  }
  else
    from_step_ = to_step_;

  long_t end_sim = from_step_ + to_do_;

  if ( min_delay_ < ( delay ) end_sim )
    to_step_ = min_delay_; // update to end of time slice
  else
    to_step_ = end_sim; // update to end of simulation time

  assert( to_step_ - from_step_ <= ( long_t ) min_delay_ );
}

void
nest::Network::print_progress_()
{
  double_t rt_factor = 0.0;

  if ( t_slice_end_.tv_sec != 0 )
  {
    long t_real_s = ( t_slice_end_.tv_sec - t_slice_begin_.tv_sec ) * 1e6;   // usec
    t_real_ += t_real_s + ( t_slice_end_.tv_usec - t_slice_begin_.tv_usec ); // usec
    double_t t_real_acc = ( t_real_ ) / 1000.;                               // ms
    double_t t_sim_acc = ( to_do_total_ - to_do_ ) * Time::get_resolution().get_ms();
    rt_factor = t_sim_acc / t_real_acc;
  }

  int_t percentage = ( 100 - int( float( to_do_ ) / to_do_total_ * 100 ) );

  std::cout << "\r" << std::setw( 3 ) << std::right << percentage << " %: "
            << "network time: " << std::fixed << std::setprecision( 1 ) << clock_.get_ms()
            << " ms, "
            << "realtime factor: " << std::setprecision( 4 ) << rt_factor
            << std::resetiosflags( std::ios_base::floatfield );
  std::flush( std::cout );
}

void
nest::Network::init_moduli_()
{
  assert( min_delay_ != 0 );
  assert( max_delay_ != 0 );

  /*
   * Ring buffers use modulos to determine where to store incoming events
   * with given time stamps, relative to the beginning of the slice in which
   * the spikes are delivered from the queue, ie, the slice after the one
   * in which they were generated. The pertaining offsets are 0..max_delay-1.
   */

  moduli_.resize( min_delay_ + max_delay_ );

  for ( delay d = 0; d < min_delay_ + max_delay_; ++d )
    moduli_[ d ] = ( clock_.get_steps() + d ) % ( min_delay_ + max_delay_ );

  // Slice-based ring-buffers have one bin per min_delay steps,
  // up to max_delay.  Time is counted as for normal ring buffers.
  // The slice_moduli_ table maps time steps to these bins
  const size_t nbuff = static_cast< size_t >(
    std::ceil( static_cast< double >( min_delay_ + max_delay_ ) / min_delay_ ) );
  slice_moduli_.resize( min_delay_ + max_delay_ );
  for ( delay d = 0; d < min_delay_ + max_delay_; ++d )
    slice_moduli_[ d ] = ( ( clock_.get_steps() + d ) / min_delay_ ) % nbuff;
}

/**
 * This function is called after all nodes have been updated.
 * We can compute the value of (T+d) mod max_delay without explicit
 * reference to the network clock, because compute_moduli_ is
 * called whenever the network clock advances.
 * The various modulos for all available delays are stored in
 * a lookup-table and this table is rotated once per time slice.
 */
void
nest::Network::compute_moduli_()
{
  assert( min_delay_ != 0 );
  assert( max_delay_ != 0 );

  /*
   * Note that for updating the modulos, it is sufficient
   * to rotate the buffer to the left.
   */
  assert( moduli_.size() == ( index )( min_delay_ + max_delay_ ) );
  std::rotate( moduli_.begin(), moduli_.begin() + min_delay_, moduli_.end() );

  /* For the slice-based ring buffer, we cannot rotate the table, but
   have to re-compute it, since max_delay_ may not be a multiple of
   min_delay_.  Reference time is the time at the beginning of the slice.
   */
  const size_t nbuff = static_cast< size_t >(
    std::ceil( static_cast< double >( min_delay_ + max_delay_ ) / min_delay_ ) );
  for ( delay d = 0; d < min_delay_ + max_delay_; ++d )
    slice_moduli_[ d ] = ( ( clock_.get_steps() + d ) / min_delay_ ) % nbuff;
}

void
nest::Network::update_delay_extrema_()
{
  min_delay_ = connection_manager_.get_min_delay().get_steps();
  max_delay_ = connection_manager_.get_max_delay().get_steps();

  if ( Communicator::get_num_processes() > 1 )
  {
    std::vector< delay > min_delays( Communicator::get_num_processes() );
    min_delays[ Communicator::get_rank() ] = min_delay_;
    Communicator::communicate( min_delays );
    min_delay_ = *std::min_element( min_delays.begin(), min_delays.end() );

    std::vector< delay > max_delays( Communicator::get_num_processes() );
    max_delays[ Communicator::get_rank() ] = max_delay_;
    Communicator::communicate( max_delays );
    max_delay_ = *std::max_element( max_delays.begin(), max_delays.end() );
  }

  if ( min_delay_ == Time::pos_inf().get_steps() )
    min_delay_ = Time::get_resolution().get_steps();
}


void
nest::Network::clear_pending_spikes()
{
  configure_spike_buffers_();
}

void
nest::Network::configure_spike_buffers_()
{
  assert( min_delay_ != 0 );

  spike_register_.clear();
  // the following line does not compile with gcc <= 3.3.5
  spike_register_.resize(
    kernel().vp_manager.get_num_threads(), std::vector< std::vector< uint_t > >( min_delay_ ) );
  for ( size_t j = 0; j < spike_register_.size(); ++j )
    for ( size_t k = 0; k < spike_register_[ j ].size(); ++k )
      spike_register_[ j ][ k ].clear();

  offgrid_spike_register_.clear();
  // the following line does not compile with gcc <= 3.3.5
  offgrid_spike_register_.resize( kernel().vp_manager.get_num_threads(),
    std::vector< std::vector< OffGridSpike > >( min_delay_ ) );
  for ( size_t j = 0; j < offgrid_spike_register_.size(); ++j )
    for ( size_t k = 0; k < offgrid_spike_register_[ j ].size(); ++k )
      offgrid_spike_register_[ j ][ k ].clear();

  // send_buffer must be >= 2 as the 'overflow' signal takes up 2 spaces.
  int send_buffer_size = kernel().vp_manager.get_num_threads() * min_delay_ > 2
    ? kernel().vp_manager.get_num_threads() * min_delay_
    : 2;
  int recv_buffer_size = send_buffer_size * Communicator::get_num_processes();
  Communicator::set_buffer_sizes( send_buffer_size, recv_buffer_size );

  // DEC cxx required 0U literal, HEP 2007-03-26
  local_grid_spikes_.clear();
  local_grid_spikes_.resize( send_buffer_size, 0U );
  local_offgrid_spikes_.clear();
  local_offgrid_spikes_.resize( send_buffer_size, OffGridSpike( 0, 0.0 ) );
  global_grid_spikes_.clear();
  global_grid_spikes_.resize( recv_buffer_size, 0U );
  global_offgrid_spikes_.clear();
  global_offgrid_spikes_.resize( recv_buffer_size, OffGridSpike( 0, 0.0 ) );

  displacements_.clear();
  displacements_.resize( Communicator::get_num_processes(), 0 );
}

void
nest::Network::create_rngs_( const bool ctor_call )
{
  // LOG(M_INFO, ) calls must not be called
  // if create_rngs_ is called from Network::Network(), since net_
  // is not fully constructed then

  // if old generators exist, remove them; since rng_ contains
  // lockPTRs, we don't have to worry about deletion
  if ( !rng_.empty() )
  {
    if ( !ctor_call )
      LOG( M_INFO, "Network::create_rngs_", "Deleting existing random number generators" );

    rng_.clear();
  }

  // create new rngs
  if ( !ctor_call )
    LOG( M_INFO, "Network::create_rngs_", "Creating default RNGs" );

  rng_seeds_.resize( kernel().vp_manager.get_num_virtual_processes() );

  for ( index i = 0; i < static_cast< index >( kernel().vp_manager.get_num_virtual_processes() );
        ++i )
  {
    unsigned long s = i + 1;
    if ( kernel().vp_manager.is_local_vp( i ) )
    {
/*
 We have to ensure that each thread is provided with a different
 stream of random numbers.  The seeding method for Knuth's LFG
 generator guarantees that different seeds yield non-overlapping
 random number sequences.

 We therefore have to seed with known numbers: using random
 seeds here would run the risk of using the same seed twice.
 For simplicity, we use 1 .. n_vps.
 */
#ifdef HAVE_GSL
      librandom::RngPtr rng( new librandom::GslRandomGen( gsl_rng_knuthran2002, s ) );
#else
      librandom::RngPtr rng = librandom::RandomGen::create_knuthlfg_rng( s );
#endif

      if ( !rng )
      {
        if ( !ctor_call )
          LOG( M_ERROR, "Network::create_rngs_", "Error initializing knuthlfg" );
        else
          std::cerr << "\nNetwork::create_rngs_\n"
                    << "Error initializing knuthlfg" << std::endl;

        throw KernelException();
      }

      rng_.push_back( rng );
    }

    rng_seeds_[ i ] = s;
  }
}

void
nest::Network::create_grng_( const bool ctor_call )
{

  // create new grng
  if ( !ctor_call )
    LOG( M_INFO, "Network::create_grng_", "Creating new default global RNG" );

// create default RNG with default seed
#ifdef HAVE_GSL
  grng_ = librandom::RngPtr(
    new librandom::GslRandomGen( gsl_rng_knuthran2002, librandom::RandomGen::DefaultSeed ) );
#else
  grng_ = librandom::RandomGen::create_knuthlfg_rng( librandom::RandomGen::DefaultSeed );
#endif

  if ( !grng_ )
  {
    if ( !ctor_call )
      LOG( M_ERROR, "Network::create_grng_", "Error initializing knuthlfg" );
    else
      std::cerr << "\nNetwork::create_grng_\n"
                << "Error initializing knuthlfg" << std::endl;

    throw KernelException();
  }

  /*
   The seed for the global rng should be different from the seeds
   of the local rngs_ for each thread seeded with 1,..., n_vps.
   */
  long s = 0;
  grng_seed_ = s;
  grng_->seed( s );
}

void
nest::Network::set_num_rec_processes( int nrp, bool called_by_reset )
{
  if ( kernel().node_manager.size() > 1 and not called_by_reset )
    throw KernelException(
      "Global spike detection mode must be enabled before nodes are created." );
  if ( nrp >= Communicator::get_num_processes() )
    throw KernelException(
      "Number of processes used for recording must be smaller than total number of processes." );
  n_rec_procs_ = nrp;
  n_sim_procs_ = Communicator::get_num_processes() - n_rec_procs_;
  create_rngs_( true );
  if ( nrp > 0 )
  {
    std::string msg = String::compose(
      "Entering global spike detection mode with %1 recording MPI processes and %2 simulating MPI "
      "processes.",
      n_rec_procs_,
      n_sim_procs_ );
    LOG( M_INFO, "Network::set_num_rec_processes", msg );
  }
}

} // end of namespace
