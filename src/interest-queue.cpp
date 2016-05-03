/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
* Copyright (c) 2016 Regents of the University of California.
*
* This file is part of the nTorrent codebase.
*
* nTorrent is free software: you can redistribute it and/or modify it under the
* terms of the GNU Lesser General Public License as published by the Free Software
* Foundation, either version 3 of the License, or (at your option) any later version.
*
* nTorrent is distributed in the hope that it will be useful, but WITHOUT ANY
* WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
* PARTICULAR PURPOSE. See the GNU Lesser General Public License for more details.
*
* You should have received copies of the GNU General Public License and GNU Lesser
* General Public License along with nTorrent, e.g., in COPYING.md file. If not, see
* <http://www.gnu.org/licenses/>.
*
* See AUTHORS for complete list of nTorrent authors and contributors.
*/

#include "interest-queue.hpp"

namespace ndn {
namespace ntorrent {

void
InterestQueue::push(shared_ptr<Interest> interest, DataCallback dataReceivedCallback,
     TimeoutCallback dataFailedCallback)
{
  m_queue.push(std::make_tuple(interest, dataReceivedCallback, dataFailedCallback));
}

queueTuple
InterestQueue::pop()
{
  queueTuple tup = m_queue.front();
  m_queue.pop();
  return tup;
}

} // namespace ntorrent
} // namespace ndn
