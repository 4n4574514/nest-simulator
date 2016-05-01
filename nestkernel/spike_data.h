/*
 *  spike_data.h
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

#ifndef SPIKE_DATA_H
#define SPIKE_DATA_H

// Includes from nestkernel:
#include "nest_types.h"

namespace nest
{

struct SpikeData
{
  unsigned int tid : 10;
  unsigned int syn_index : 6;
  unsigned int lcid : 25;
  unsigned int lag : 6;
  unsigned int marker : 2;
  const static unsigned int end_marker; // 1
  const static unsigned int complete_marker; // 2
  const static unsigned int invalid_marker; // 3
  SpikeData();
  SpikeData( const thread tid, const unsigned int syn_index, const unsigned int lcid, const unsigned int lag );
  void set( const thread tid, const unsigned int syn_index, const unsigned int lcid, const unsigned int lag );
  void reset_marker();
  void set_complete_marker();
  void set_end_marker();
  void set_invalid_marker();
  bool is_complete_marker() const;
  bool is_end_marker() const;
  bool is_invalid_marker() const;
};

inline
SpikeData::SpikeData()
  : tid( 0 )
  , syn_index( 0 )
  , lcid( 0 )
  , lag( 0 )
  , marker( 0 )
{
}

inline
SpikeData::SpikeData( const thread tid, const unsigned int syn_index, const unsigned int lcid, const unsigned int lag )
  : tid( tid )
  , syn_index( syn_index )
  , lcid( lcid )
  , lag( lag )
  , marker( 0 )
{
}

inline void
SpikeData::set( const thread tid, const unsigned int syn_index, const unsigned int lcid, const unsigned int lag )
{
  (*this).tid = tid;
  (*this).syn_index = syn_index;
  (*this).lcid = lcid;
  (*this).lag = lag;
  marker = 0;
}

inline void
SpikeData::reset_marker()
{
  marker = 0;
}

inline void
SpikeData::set_complete_marker()
{
  marker = complete_marker;
}

inline void
SpikeData::set_end_marker()
{
  marker = end_marker;
}

inline void
SpikeData::set_invalid_marker()
{
  marker = invalid_marker;
}

inline bool
SpikeData::is_complete_marker() const
{
  return marker == complete_marker;
}

inline bool
SpikeData::is_end_marker() const
{
  return marker == end_marker;
}

inline bool
SpikeData::is_invalid_marker() const
{
  return marker == invalid_marker;
}

} // namespace nest

#endif /* SPIKE_DATA_H */
