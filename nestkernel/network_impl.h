/*
 *  network_impl.h
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

#ifndef NETWORK_IMPL_H
#define NETWORK_IMPL_H

#include "network.h"
#include "conn_builder.h"
#include "conn_builder_factory.h"
#include "kernel_manager.h"

namespace nest
{

template < class EventT >
inline
void
Network::send( Node& source, EventT& e, const long_t lag )
{
  e.set_stamp( kernel().simulation_manager.get_slice_origin() + Time::step( lag + 1 ) );
  e.set_sender( source );
  thread t = source.get_thread();
  index gid = source.get_gid();

  assert( !source.has_proxies() );
  connection_manager_.send( t, gid, e );
}

template <>
inline
void
Network::send< SpikeEvent >( Node& source, SpikeEvent& e, const long_t lag )
{
  e.set_stamp( kernel().simulation_manager.get_slice_origin() + Time::step( lag + 1 ) );
  e.set_sender( source );
  thread t = source.get_thread();

  if ( source.has_proxies() )
  {
    if ( source.is_off_grid() )
      send_offgrid_remote( t, e, lag );
    else
      send_remote( t, e, lag );
  }
  else
    send_local( t, source, e );
}

template <>
inline
void
Network::send< DSSpikeEvent >( Node& source, DSSpikeEvent& e, const long_t lag )
{
  e.set_stamp( kernel().simulation_manager.get_slice_origin() + Time::step( lag + 1 ) );
  e.set_sender( source );
  thread t = source.get_thread();

  assert( !source.has_proxies() );
  send_local( t, source, e );
}

template < typename ConnBuilder >
void
Network::register_conn_builder( const std::string& name )
{
  assert( !connruledict_->known( name ) );
  GenericConnBuilderFactory* cb = new ConnBuilderFactory< ConnBuilder >();
  assert( cb != 0 );
  const int id = connbuilder_factories_.size();
  connbuilder_factories_.push_back( cb );
  connruledict_->insert( name, id );
}

}

#endif
