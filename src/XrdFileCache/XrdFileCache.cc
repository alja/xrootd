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

#include <fcntl.h>
#include <sstream>
#include <sys/statvfs.h>

#include "XrdCl/XrdClConstants.hh"
#include "XrdSys/XrdSysPthread.hh"
#include "XrdOss/XrdOss.hh"
#include "XrdOuc/XrdOucEnv.hh"

#include "XrdFileCache.hh"
#include "XrdFileCachePrefetch.hh"
#include "XrdFileCacheIOEntireFile.hh"
#include "XrdFileCacheIOFileBlock.hh"
#include "XrdFileCacheFactory.hh"
#include "XrdFileCachePrefetch.hh"


XrdSysCondVar XrdFileCache::Cache::m_writeMutex(0);
std::queue<XrdFileCache::Cache::WriteTask> XrdFileCache::Cache::m_writeQueue;


using namespace XrdFileCache;
void *ProcessWriteTaskThread(void* c)
{
   Cache *cache = static_cast<Cache*>(c);
   cache->ProcessWriteTasks();
   return NULL;
}

Cache::Cache(XrdOucCacheStats & stats)
   : m_attached(0),
     m_stats(stats)
     //   m_writeMutex(0)
{
   pthread_t tid;
   XrdSysThread::Run(&tid, ProcessWriteTaskThread, (void*)this, 0, "XrdFileCache WriteTasks ");
}

XrdOucCacheIO *Cache::Attach(XrdOucCacheIO *io, int Options)
{
   if (Factory::GetInstance().Decide(io))
   {
      XrdSysMutexHelper lock(&m_io_mutex);

      m_attached++;

      clLog()->Info(XrdCl::AppMsg, "Cache::Attach() %s", io->Path());

      if (io)
      {
         if (Factory::GetInstance().RefConfiguration().m_prefetchFileBlocks)
            return new IOFileBlock(*io, m_stats, *this);
         else
            return new IOEntireFile(*io, m_stats, *this);
      }
      else
      {
         clLog()->Debug(XrdCl::AppMsg, "Cache::Attache(), XrdOucCacheIO == NULL %s", io->Path());
      }

      m_attached--;
   }
   return io;
}

int Cache::isAttached()
{
   XrdSysMutexHelper lock(&m_io_mutex);
   return m_attached;
}

void Cache::Detach(XrdOucCacheIO* io)
{
   clLog()->Info(XrdCl::AppMsg, "Cache::Detach() %s", io->Path());

   XrdSysMutexHelper lock(&m_io_mutex);
   m_attached--;

   clLog()->Debug(XrdCl::AppMsg, "Cache::Detach(), deleting IO object. Attach count = %d %s", m_attached, io->Path());

   delete io;
}


bool Cache::getFilePathFromURL(const char* url, std::string &result) const
{
   std::string path = url;
   size_t split_loc = path.rfind("//");

   if (split_loc == path.npos)
      return false;

   size_t kloc = path.rfind("?");

   if (kloc == path.npos)
      return false;

   result = Factory::GetInstance().RefConfiguration().m_cache_dir;
   result += path.substr(split_loc+1,kloc-split_loc-1);

   return true;
}

//______________________________________________________________________________
bool
Cache::HaveFreeWritingSlots() 
{
   const static size_t maxWriteWaits=10000;
   return m_writeQueue.size() < maxWriteWaits;
}


//______________________________________________________________________________
void
Cache::AddWriteTask(Prefetch* p, int ri, int fi, size_t s)
{
   XrdCl::DefaultEnv::GetLog()->Debug(XrdCl::AppMsg, "Cache::AddWriteTask() bi=%d,  fi=%d, size= %d", ri, fi, (int) s);
   XrdSysCondVarHelper xx(m_writeMutex);
   m_writeQueue.push(WriteTask(p, ri, fi, s));
   m_writeMutex.Signal();
}

//______________________________________________________________________________
void  
Cache::ProcessWriteTasks()
{
   while (true)
   {
      m_writeMutex.Lock();
      if (m_writeQueue.empty())
      {
         m_writeMutex.Wait();
      }
      WriteTask t = m_writeQueue.front();
      m_writeQueue.pop();
      m_writeMutex.UnLock();
      t.prefetch->WriteBlockToDisk(t.ramBlockIdx, t.fileBlockIdx, t.size);
   }
}
