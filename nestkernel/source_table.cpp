/*
 *  source_table.cpp
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

// Includes from nestkernel:
#include "connection_manager_impl.h"
#include "node_manager_impl.h"
#include "kernel_manager.h"
#include "source_table.h"
#include "vp_manager_impl.h"

nest::SourceTable::SourceTable()
{
}

nest::SourceTable::~SourceTable()
{
}

void
nest::SourceTable::initialize()
{
  assert( sizeof( Source ) == 8 );
  const thread num_threads = kernel().vp_manager.get_num_threads();
  synapse_ids_.resize( num_threads );
  sources_.resize( num_threads );
  is_cleared_.resize( num_threads );
  saved_entry_point_.resize( num_threads );
  current_positions_.resize( num_threads );
  saved_positions_.resize( num_threads );

  for ( thread tid = 0; tid < num_threads; ++tid )
  {
    synapse_ids_[ tid ] = new std::map< synindex, synindex >();
    sources_[ tid ] = new std::vector< std::vector< Source >* >( 0 );
    current_positions_[ tid ] = new SourceTablePosition();
    saved_positions_[ tid ] = new SourceTablePosition();
    is_cleared_[ tid ] = false;
    saved_entry_point_[ tid ] = false;
  }
}

void
nest::SourceTable::finalize()
{
  for ( std::vector< std::map< synindex, synindex >* >::iterator it =
          synapse_ids_.begin();
        it != synapse_ids_.end();
        ++it )
  {
    delete *it;
  }
  synapse_ids_.clear();
  if ( not is_cleared() )
  {
    for ( size_t tid = 0; tid < sources_.size(); ++tid )
    {
      clear( tid );
    }
  }
  for ( std::vector< std::vector< std::vector< Source >* >* >::iterator it =
          sources_.begin();
        it != sources_.end();
        ++it )
  {
    delete *it;
  }
  sources_.clear();
  for ( std::vector< SourceTablePosition* >::iterator it =
          current_positions_.begin();
        it != current_positions_.end();
        ++it )
  {
    delete *it;
  }
  current_positions_.clear();
  for (
    std::vector< SourceTablePosition* >::iterator it = saved_positions_.begin();
    it != saved_positions_.end();
    ++it )
  {
    delete *it;
  }
  saved_positions_.clear();
}

bool
nest::SourceTable::is_cleared() const
{
  bool all_cleared = true;
  // we only return true, if is_cleared is true for all threads
  for ( thread tid = 0; tid < kernel().vp_manager.get_num_threads(); ++tid )
  {
    all_cleared &= is_cleared_[ tid ];
  }
  return all_cleared;
}

std::vector< std::vector< nest::Source >* >&
nest::SourceTable::get_thread_local_sources( const thread tid )
{
  return *sources_[ tid ];
}

nest::SourceTablePosition
nest::SourceTable::find_maximal_position() const
{
  SourceTablePosition max_position( -1, -1, -1 );
  for ( thread tid = 0; tid < kernel().vp_manager.get_num_threads(); ++tid )
  {
    if ( max_position < ( *saved_positions_[ tid ] ) )
    {
      max_position = ( *saved_positions_[ tid ] );
    }
  }
  return max_position;
}

void
nest::SourceTable::clean( const thread tid )
{
  // find maximal position in source table among threads to make sure
  // unprocessed entries are not removed. given this maximal position,
  // we can savely delete all larger entries since they will not be
  // touched any more.
  const SourceTablePosition max_position = find_maximal_position();
  // we need to distinguish whether we are in the vector corresponding
  // to max position or above. we can delete all entries above the
  // maximal position, otherwise we need to respect to indices.
  if ( max_position.tid == tid )
  {
    for ( synindex syn_index = max_position.syn_index;
          syn_index < ( *sources_[ tid ] ).size();
          ++syn_index )
    {
      std::vector< Source >& sources = *( *sources_[ tid ] )[ syn_index ];
      if ( max_position.syn_index == syn_index )
      {
        // we need to add 1 to max_position.lcid since
        // max_position.lcid can contain a valid entry which we do not
        // want to delete.
        if ( max_position.lcid + 1 < static_cast< long >( sources.size() ) )
        {
          const size_t deleted_elements =
            sources.end() - ( sources.begin() + max_position.lcid + 1 );
          sources.erase(
            sources.begin() + max_position.lcid + 1, sources.end() );
          if ( deleted_elements > min_deleted_elements_ )
          {
            std::vector< Source >( sources.begin(), sources.end() )
              .swap( sources );
          }
        }
      }
      else
      {
        const size_t deleted_elements = sources.end() - sources.begin();
        sources.erase( sources.begin(), sources.end() );
        if ( deleted_elements > min_deleted_elements_ )
        {
          std::vector< Source >( sources.begin(), sources.end() )
            .swap( sources );
        }
      }
    }
  }
  else if ( max_position.tid < tid )
  {
    for ( synindex syn_index = 0; syn_index < ( *sources_[ tid ] ).size();
          ++syn_index )
    {
      std::vector< Source >& sources = *( *sources_[ tid ] )[ syn_index ];
      const size_t deleted_elements = sources.end() - sources.begin();
      sources.erase( sources.begin(), sources.end() );
      if ( deleted_elements > min_deleted_elements_ )
      {
        std::vector< Source >( sources.begin(), sources.end() ).swap( sources );
      }
    }
  }
}

void
nest::SourceTable::reserve( const thread tid,
  const synindex syn_id,
  const size_t count )
{
  std::map< synindex, synindex >::iterator it =
    synapse_ids_[ tid ]->find( syn_id );
  // if this synapse type is not known yet, create entry for new synapse vector
  if ( it == synapse_ids_[ tid ]->end() )
  {
    const index prev_n_synapse_types = synapse_ids_[ tid ]->size();
    ( *synapse_ids_[ tid ] )[ syn_id ] = prev_n_synapse_types;
    sources_[ tid ]->resize( prev_n_synapse_types + 1 );
    ( *sources_[ tid ] )[ prev_n_synapse_types ] =
      new std::vector< Source >( 0 );
    ( *sources_[ tid ] )[ prev_n_synapse_types ]->reserve( count );
  }
  // otherwise we can directly reserve
  else
  {
    ( *sources_[ tid ] )[ it->second ]->reserve( count );
  }
}

void
nest::SourceTable::compute_buffer_pos_for_unique_secondary_sources( std::map< index, size_t >& gid_to_buffer_pos ) const
{
  // collect all unique sources
  std::set< std::pair< index, size_t > > unique_secondary_sources_set;

#pragma omp parallel shared( unique_secondary_sources_set )
  {
    const thread tid = kernel().vp_manager.get_thread_id();
    for ( size_t syn_index = 0; syn_index < sources_[ tid ]->size();
          ++syn_index )
    {
      const synindex syn_id =
        kernel().connection_manager.get_syn_id( tid, syn_index );
      if ( not kernel()
                 .model_manager.get_synapse_prototype( syn_id, tid )
                 .is_primary() )
      {
        const size_t event_size =
          kernel()
          .model_manager.get_secondary_event_prototype( syn_id, tid )
          .prototype_size();
        for ( std::vector< Source >::const_iterator cit =
                ( *sources_[ tid ] )[ syn_index ]->begin();
              cit != ( *sources_[ tid ] )[ syn_index ]->end();
              ++cit )
        {
#pragma omp critical
          {
            unique_secondary_sources_set.insert( std::pair< index, size_t >( cit->gid, event_size ) );
          }
        }
      }
    }
  } // of omp parallel

  // given all unique sources, calculate maximal chunksize per rank
  // and fill vector of unique sources
  std::vector< size_t > count_per_rank(
    kernel().mpi_manager.get_num_processes(), 0 );
  size_t i = 0;
  for ( std::set< std::pair< index, size_t > >::const_iterator cit = unique_secondary_sources_set.cbegin();
        cit != unique_secondary_sources_set.cend(); ++cit )
  {
    count_per_rank[ kernel().node_manager.get_process_id_of_gid( cit->first ) ] += cit->second;
    ++i;
  }

  // determine maximal chunksize across all MPI ranks
  std::vector< size_t > max_count(
    1, *std::max_element( count_per_rank.begin(), count_per_rank.end() ) );
  kernel().mpi_manager.communicate_Allreduce_max_in_place( max_count );

  const size_t secondary_buffer_chunk_size = max_count[ 0 ] + 1;

  kernel().mpi_manager.set_chunk_size_secondary_events(
    secondary_buffer_chunk_size );

  // offsets in receive buffer
  std::vector< size_t > buffer_position_by_rank(
    kernel().mpi_manager.get_num_processes(), 0 );
  for ( size_t i = 0; i < buffer_position_by_rank.size(); ++i )
  {
    buffer_position_by_rank[ i ] = i * secondary_buffer_chunk_size;
  }

  for ( std::set< std::pair< index, size_t > >::const_iterator cit = unique_secondary_sources_set.cbegin();
        cit != unique_secondary_sources_set.cend(); ++cit )
  {
    const thread target_rank = kernel().node_manager.get_process_id_of_gid( cit->first );
    gid_to_buffer_pos.insert( std::pair< index, size_t >( cit->first, buffer_position_by_rank[ target_rank ] ) );
    buffer_position_by_rank[ target_rank ] += cit->second;
  }
}
