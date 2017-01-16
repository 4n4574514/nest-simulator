/*
 *  source_table_impl.h
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

#ifndef SOURCE_TABLE_IMPL_H
#define SOURCE_TABLE_IMPL_H

// C++ includes
#include <algorithm> // for std::max_element

// Includes from nestkernel:
#include "connection_manager_impl.h"
#include "event_impl.h"
#include "kernel_manager.h"
#include "mpi_manager.h"
#include "node_manager_impl.h"
#include "source_table.h"
#include "target_table.h"

namespace nest
{

inline bool
SourceTable::get_next_target_data( const thread tid,
  const thread rank_start,
  const thread rank_end,
  thread& target_rank,
  TargetData& next_target_data )
{
  SourceTablePosition& current_position = *current_positions_[ tid ];
  // we stay in this loop either until we can return a valid
  // TargetData object or we have reached the end of the sources table
  while ( true )
  {
    // check for validity of indices and update if necessary
    if ( current_position.lcid < 0 )
    {
      --current_position.syn_index;
      if ( current_position.syn_index >= 0 )
      {
        current_position.lcid =
          ( *sources_[ current_position.tid ] )[ current_position.syn_index ]
            ->size() - 1;
        continue;
      }
      else
      {
        --current_position.tid;
        if ( current_position.tid >= 0 )
        {
          current_position.syn_index =
            ( *sources_[ current_position.tid ] ).size() - 1;
          if ( current_position.syn_index >= 0 )
          {
            current_position.lcid =
              ( *sources_[ current_position.tid ] )[ current_position
                                                       .syn_index ]->size() - 1;
          }
          continue;
        }
        else
        {
          assert( current_position.tid < 0 );
          assert( current_position.syn_index < 0 );
          assert( current_position.lcid < 0 );
          return false; // reached the end of the sources table
        }
      }
    }

    if ( current_position.lcid
        < static_cast< long >(
            ( *last_sorted_source_[ current_position.tid ] )[ current_position
                                                                .syn_index ] )
      && ( *last_sorted_source_[ current_position.tid ] )[ current_position
                                                             .syn_index ]
        < ( *( *sources_[ current_position.tid ] )[ current_position
                                                      .syn_index ] ).size() )
    {
      return false;
    }

    // the current position contains an entry, so we retrieve it
    Source& current_source =
      ( *( *sources_[ current_position.tid ] )[ current_position.syn_index ] )
        [ current_position.lcid ];
    if ( current_source.is_processed() || current_source.is_disabled() )
    {
      // looks like we've processed this already, let's
      // continue
      --current_position.lcid;
      continue;
    }

    // TODO@5g: this really is the source rank, isn't it? rename?
    target_rank =
      kernel().node_manager.get_process_id_of_gid( current_source.get_gid() );
    // now we need to determine whether this thread is
    // responsible for this part of the MPI buffer; if not we
    // just continue with the next iteration of the loop
    if ( target_rank < rank_start || target_rank >= rank_end )
    {
      --current_position.lcid;
      continue;
    }

    // we have found a valid entry, so mark it as processed
    current_source.set_processed( true );

    // we need to set the marker whether the entry following this
    // entry, if existent, has the same source
    kernel().connection_manager.set_has_source_subsequent_targets(
      current_position.tid,
      current_position.syn_index,
      current_position.lcid,
      false );
    if ( ( current_position.lcid + 1
             < static_cast< long >(
                 ( *sources_[ current_position.tid ] )[ current_position
                                                          .syn_index ]->size() )
           && ( *( *sources_[ current_position.tid ] )
                  [ current_position.syn_index ] )[ current_position.lcid + 1 ]
                .get_gid() == current_source.get_gid() ) )
    {
      kernel().connection_manager.set_has_source_subsequent_targets(
        current_position.tid,
        current_position.syn_index,
        current_position.lcid,
        true );
    }

    // we decrease the counter without returning a TargetData if the
    // entry preceeding this entry has the same source, but only if it
    // was not processed yet
    if ( current_position.lcid - 1 > -1
      && ( *( *sources_[ current_position.tid ] )
             [ current_position.syn_index ] )[ current_position.lcid - 1 ].get_gid()
        == current_source.get_gid()
      && not( *( *sources_[ current_position.tid ] )
                [ current_position.syn_index ] )[ current_position.lcid - 1 ]
              .is_processed() )
    {
      --current_position.lcid;
      continue;
    }

    // otherwise we return a valid TargetData
    else
    {
      // set values of next_target_data
      next_target_data.set_lid(
        kernel().vp_manager.gid_to_lid( current_source.get_gid() ) );
      next_target_data.set_tid( kernel().vp_manager.vp_to_thread(
        kernel().vp_manager.suggest_vp( current_source.get_gid() ) ) );
      if ( current_source.is_primary() )
      {
        next_target_data.is_primary( true );
        // we store the thread index of the sources table, not our own tid
        next_target_data.get_target().set_tid( current_position.tid );
        next_target_data.get_target().set_rank(
          kernel().mpi_manager.get_rank() );
        next_target_data.get_target().set_processed( false );
        next_target_data.get_target().set_syn_index(
          current_position.syn_index );
        next_target_data.get_target().set_lcid( current_position.lcid );
      }
      else
      {
        next_target_data.is_primary( false );
        const size_t recv_buffer_pos =
          kernel().connection_manager.get_secondary_recv_buffer_position(
            current_position.tid,
            current_position.syn_index,
            current_position.lcid );
        const size_t send_buffer_pos =
          kernel().mpi_manager.get_rank() * kernel().mpi_manager.get_chunk_size_secondary_events()
          + ( recv_buffer_pos - target_rank * kernel().mpi_manager.get_chunk_size_secondary_events() );
        reinterpret_cast< SecondaryTargetData* >( &next_target_data )
          ->set_send_buffer_pos( send_buffer_pos );
      }
      --current_position.lcid;
      return true; // found a valid entry
    }
  }
}

} // namespace nest

#endif /* SOURCE_TABLE_IMPL_H */
