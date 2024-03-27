#ifndef __XRDPFC_STATS_HH__
#define __XRDPFC_STATS_HH__

//----------------------------------------------------------------------------------
// Copyright (c) 2014 by Board of Trustees of the Leland Stanford, Jr., University
// Author: Alja Mrak-Tadel, Matevz Tadel, Brian Bockelman
//----------------------------------------------------------------------------------
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//----------------------------------------------------------------------------------

namespace XrdPfc
{

//----------------------------------------------------------------------------
//! Statistics of cache utilisation by a File object.
//  Used both as aggregation of usage by a single file as well as for
//  collecting per-directory statistics on time-interval basis. In this second
//  case they are used as "deltas" ... differences in respect to a previous
//  reference value.
//  For running averages / deltas, one might need a version with doubles, so
//  it might make sense to template this. And add some timestamp.
//----------------------------------------------------------------------------
class Stats
{
public:
   int       m_NumIos = 0;          //!< number of IO objects attached during this access
   int       m_Duration = 0;        //!< total duration of all IOs attached
   long long m_BytesHit = 0;        //!< number of bytes served from disk
   long long m_BytesMissed = 0;     //!< number of bytes served from remote and cached
   long long m_BytesBypassed = 0;   //!< number of bytes served directly through XrdCl
   long long m_BytesWritten = 0;    //!< number of bytes written to disk
   int       m_NCksumErrors = 0;    //!< number of checksum errors while getting data from remote

   //----------------------------------------------------------------------

   Stats() = default;

   Stats(const Stats& s) = default;

   Stats& operator=(const Stats&) = default;

   //----------------------------------------------------------------------

   void AddReadStats(const Stats &s)
   {
      m_BytesHit      += s.m_BytesHit;
      m_BytesMissed   += s.m_BytesMissed;
      m_BytesBypassed += s.m_BytesBypassed;
   }

   void AddBytesHit(long long bh)
   {
      m_BytesHit      += bh;
   }

   void AddWriteStats(long long bytes_written, int n_cks_errs)
   {
      m_BytesWritten += bytes_written;
      m_NCksumErrors += n_cks_errs;
   }

   void IoAttach()
   {
      ++m_NumIos;
   }

   void IoDetach(int duration)
   {
      m_Duration += duration;
   }


   //----------------------------------------------------------------------

   void DeltaToReference(const Stats& ref)
   {
      m_NumIos        = ref.m_NumIos        - m_NumIos;
      m_Duration      = ref.m_Duration      - m_Duration;
      m_BytesHit      = ref.m_BytesHit      - m_BytesHit;
      m_BytesMissed   = ref.m_BytesMissed   - m_BytesMissed;
      m_BytesBypassed = ref.m_BytesBypassed - m_BytesBypassed;
      m_BytesWritten  = ref.m_BytesWritten  - m_BytesWritten;
      m_NCksumErrors  = ref.m_NCksumErrors  - m_NCksumErrors;
   }

   void AddUp(const Stats& s)
   {
      m_NumIos        += s.m_NumIos;
      m_Duration      += s.m_Duration;
      m_BytesHit      += s.m_BytesHit;
      m_BytesMissed   += s.m_BytesMissed;
      m_BytesBypassed += s.m_BytesBypassed;
      m_BytesWritten  += s.m_BytesWritten;
      m_NCksumErrors  += s.m_NCksumErrors;
   }

   void Reset()
   {
      m_NumIos        = 0;
      m_Duration      = 0;
      m_BytesHit      = 0;
      m_BytesMissed   = 0;
      m_BytesBypassed = 0;
      m_BytesWritten  = 0;
      m_NCksumErrors  = 0;
   }
};

//==============================================================================

class DirStats : public Stats
{
public:
   long long m_BytesRemoved = 0;
   int       m_NFilesOpened = 0;
   int       m_NFilesClosed = 0;
   int       m_NFilesCreated = 0;
   int       m_NFilesRemoved = 0; // purged
   int       m_NDirectoriesCreated = 0; // this is hard, oss does it for us ... but we MUSt know it for DirState creation.
   int       m_NDirectoriesRemoved = 0;

   //----------------------------------------------------------------------

   DirStats() = default;

   DirStats(const DirStats& s) = default;

   DirStats& operator=(const DirStats&) = default;

   //----------------------------------------------------------------------

   // maybe some missing AddSomething functions, like for read/write

   //----------------------------------------------------------------------

   using Stats::DeltaToReference; // activate overload based on arg
   void DeltaToReference(const DirStats& ref)
   {
      Stats::DeltaToReference(ref);
      m_BytesRemoved        = ref.m_BytesRemoved        - m_BytesRemoved;
      m_NFilesOpened        = ref.m_NFilesOpened        - m_NFilesOpened;
      m_NFilesClosed        = ref.m_NFilesClosed        - m_NFilesClosed;
      m_NFilesCreated       = ref.m_NFilesCreated       - m_NFilesCreated;
      m_NFilesRemoved       = ref.m_NFilesRemoved       - m_NFilesRemoved;
      m_NDirectoriesCreated = ref.m_NDirectoriesCreated - m_NDirectoriesCreated;
      m_NDirectoriesRemoved = ref.m_NDirectoriesRemoved - m_NDirectoriesRemoved;
   }

   using Stats::AddUp; // activate overload based on arg
   void AddUp(const DirStats& s)
   {
      Stats::AddUp(s);
      m_BytesRemoved        += s.m_BytesRemoved;
      m_NFilesOpened        += s.m_NFilesOpened;
      m_NFilesClosed        += s.m_NFilesClosed;
      m_NFilesCreated       += s.m_NFilesCreated;
      m_NFilesRemoved       += s.m_NFilesRemoved;
      m_NDirectoriesCreated += s.m_NDirectoriesCreated;
      m_NDirectoriesRemoved += s.m_NDirectoriesRemoved;
   }
};

}

#endif
