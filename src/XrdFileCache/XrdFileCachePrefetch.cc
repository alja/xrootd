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

#include <stdio.h>
#include <sstream>
#include <fcntl.h>

#include "XrdCl/XrdClLog.hh"
#include "XrdCl/XrdClConstants.hh"
#include "XrdOss/XrdOss.hh"
#include "XrdCl/XrdClFile.hh"
#include "XrdSys/XrdSysPthread.hh"
#include "XrdOuc/XrdOucEnv.hh"


#include "XrdFileCachePrefetch.hh"
#include "XrdFileCacheFactory.hh"
#include "XrdFileCache.hh"

using namespace XrdFileCache;

Prefetch::RAM::RAM(): m_numBlocks(0),m_buffer(0),  m_blockStates(0)
{
   // AMT this is temp, should be parsed in configuration
   m_numBlocks = 32;
   m_buffer = (char*)malloc(m_numBlocks * 1024* 1024);
   m_blockStates = new bool[m_numBlocks];

   for (int i=0; i < m_numBlocks; ++i) m_blockStates[i] = 0;
}

Prefetch::RAM::~RAM()
{
   free(m_buffer);
   delete [] m_blockStates;
}

Prefetch::Prefetch(XrdOucCacheIO &inputIO, std::string& disk_file_path, long long iOffset, long long iFileSize) :
   m_output(NULL),
   m_infoFile(NULL),
   m_input(inputIO),
   m_temp_filename(disk_file_path),
   m_offset(iOffset),
   m_fileSize(iFileSize),
   m_started(false),
   m_failed(false),
   m_stop(false),
   m_stateCond(0),    // We will explicitly lock the condition before use.
   m_quequeMutex(0)
{
   clLog()->Debug(XrdCl::AppMsg, "Prefetch::Prefetch() %s", m_input.Path());
}

//______________________________________________________________________________
Prefetch::~Prefetch()
{
   // see if we have to shut down
   m_downloadStatusMutex.Lock();
   m_cfi.CheckComplete();
   m_downloadStatusMutex.UnLock();

   if (m_started == false) return;

   if (m_cfi.IsComplete() == false)
   {
      clLog()->Info(XrdCl::AppMsg, "Prefetch::~Prefetch() file not complete... %s", m_input.Path());
      fflush(stdout);
      XrdSysCondVarHelper monitor(m_stateCond);
      if (m_stop == false)
      {
         m_stop = true;
         clLog()->Info(XrdCl::AppMsg, "Prefetch::~Prefetch() waiting to stop Run() thread ... %s", m_input.Path());
         m_stateCond.Wait();
      }
   }

   // write statistics in *cinfo file
   AppendIOStatToFileInfo();

   clLog()->Info(XrdCl::AppMsg, "Prefetch::~Prefetch close data file %s", m_input.Path());

   if (m_output)
   {
      m_output->Close();
      delete m_output;
      m_output = NULL;
   }
   if (m_infoFile)
   {
      RecordDownloadInfo();
      clLog()->Info(XrdCl::AppMsg, "Prefetch::~Prefetch close info file %s", m_input.Path());

      m_infoFile->Close();
      delete m_infoFile;
      m_infoFile = NULL;
   }
}

//______________________________________________________________________________

bool Prefetch::Open()
{
   clLog()->Debug(XrdCl::AppMsg, "Prefetch::Open() open file for disk cache %s", m_input.Path());
   XrdOss  &m_output_fs =  *Factory::GetInstance().GetOss();
   // Create the data file itself.
   XrdOucEnv myEnv;
   m_output_fs.Create(Factory::GetInstance().RefConfiguration().m_username.c_str(), m_temp_filename.c_str(), 0600, myEnv, XRDOSS_mkpath);
   m_output = m_output_fs.newFile(Factory::GetInstance().RefConfiguration().m_username.c_str());
   if (m_output)
   {
      int res = m_output->Open(m_temp_filename.c_str(), O_RDWR, 0600, myEnv);
      if ( res < 0)
      {
         clLog()->Error(XrdCl::AppMsg, "Prefetch::Open() can't get data-FD for %s %s", m_temp_filename.c_str(), m_input.Path());
         delete m_output;
         m_output = NULL;
         return false;
      }
   }
   // Create the info file
   std::string ifn = m_temp_filename + Info::m_infoExtension;
   m_output_fs.Create(Factory::GetInstance().RefConfiguration().m_username.c_str(), ifn.c_str(), 0600, myEnv, XRDOSS_mkpath);
   m_infoFile = m_output_fs.newFile(Factory::GetInstance().RefConfiguration().m_username.c_str());
   if (m_infoFile)
   {

      int res = m_infoFile->Open(ifn.c_str(), O_RDWR, 0600, myEnv);
      if ( res < 0 )
      {
         clLog()->Error(XrdCl::AppMsg, "Prefetch::Open() can't get info-FD %s  %s", ifn.c_str(), m_input.Path());
         delete m_output;
         m_output = NULL;
         delete m_infoFile;
         m_infoFile = NULL;

         return false;
      }
   }
   if ( m_cfi.Read(m_infoFile) <= 0)
   {
      assert(m_fileSize > 0);
      int ss = (m_fileSize -1)/m_cfi.GetBufferSize() + 1;
      clLog()->Info(XrdCl::AppMsg, "Creating new file info with size %lld. Reserve space for %d blocks %s", m_fileSize,  ss, m_input.Path());
      m_cfi.ResizeBits(ss);
      RecordDownloadInfo();
   }
   else
   {
      clLog()->Debug(XrdCl::AppMsg, "Info file already exists %s", m_input.Path());
      // m_cfi.Print();
   }

   return true;
}


//_________________________________________________________________________________________________
void 
Prefetch::Run()
{
   {
      XrdSysCondVarHelper monitor(m_stateCond);
      if (m_started)
      {
         return;
      }

      if ( !Open())
      {
         m_failed = true;
      }
      m_started = true;
      // Broadcast to possible io-read waiting objects
      m_stateCond.Broadcast();

      if (m_failed) return;
      
   }
   assert(m_infoFile);
   clLog()->Debug(XrdCl::AppMsg, "Prefetch::Run() %s", m_input.Path());


   Task task;
   int numReadBlocks =0;
   while (GetNextTask(task))
   { 
      bool already;
      m_downloadStatusMutex.Lock();
      already = m_cfi.TestBit(task.fileBlockIdx);
      m_downloadStatusMutex.UnLock();
      if (already)
      {
         clLog()->Debug(XrdCl::AppMsg, "Prefetch::Run() block [%d] already done, continue ... %s",task.fileBlockIdx , m_input.Path());
         continue;
      } else {
         clLog()->Dump(XrdCl::AppMsg, "Prefetch::Run() download block [%d] %s", task.fileBlockIdx, m_input.Path());
      }

      DoTask(task);
      numReadBlocks++;
      if (numReadBlocks % 10)
         RecordDownloadInfo();

      // after completing a task, check if IO wants to break
      if (m_stop)
      {
         clLog()->Debug(XrdCl::AppMsg, "stopping for a clean cause %s", m_input.Path());
         m_stateCond.Signal();
         return;
      }

   }  // loop tasks
   m_cfi.CheckComplete();
   clLog()->Debug(XrdCl::AppMsg, "Prefetch::Run() exits, download %s  !", m_cfi.IsComplete() ? " completed " : "unfinished %s", m_input.Path());


   RecordDownloadInfo();
} // end Run()

//_____________________________________________________________________________
bool
Prefetch::GetNextTask(Task& t)
{
   m_quequeMutex.Lock();

   if (m_tasks_queue.empty())
   {
      if (m_quequeMutex.WaitMS(500))
      {
         m_quequeMutex.UnLock(); 

         m_downloadStatusMutex.Lock();
         for (int i = 0; i < m_cfi.GetSizeInBits(); ++i)
         {
            if (m_cfi.TestBit(i) == false)
            {
               t.fileBlockIdx = i;
               t.condVar = 0;
               break;
            }
         }
         m_downloadStatusMutex.UnLock();

         clLog()->Dump(XrdCl::AppMsg, "Prefetch::GetNextTask() read first undread block %s", m_input.Path());
         if (t.fileBlockIdx >= 0 ) {
            return ReadBlockFromTask(t.fileBlockIdx, NULL, t.fileBlockIdx*m_cfi.GetBufferSize(), size_t(m_cfi.GetBufferSize()));
         }
      }
      return false;
   }
   else 
   {
      clLog()->Debug(XrdCl::AppMsg, "Prefetch::GetNextTask() from queue %s", m_input.Path());
      t = m_tasks_queue.front();
      m_tasks_queue.pop();
      m_quequeMutex.UnLock(); 
      return true;
   }
}

//______________________________________________________________________________
void
Prefetch::DoTask(Task& task)
{ 
   const static int PREFETCH_MAX_ATTEMPTS = 10;
   // read block into buffer

   long long offset = task.fileBlockIdx * m_cfi.GetBufferSize();
   int missing =  task.size;
   int cnt = 0;
   char* buff = m_ram.m_buffer;
   buff += task.ramBlockIdx * m_cfi.GetBufferSize();
   while (missing)
   {
      int retval = m_input.Read(buff, offset + m_offset, missing);
      if (retval < 0)
      {
         clLog()->Warning(XrdCl::AppMsg, "Prefetch::DoTask() Failed for negative ret %d block %d %s", retval, task.fileBlockIdx , m_input.Path());
         XrdSysCondVarHelper monitor(m_stateCond);
         m_failed = true;
         retval = -EINTR;
         break;
      }

      missing -= retval;
      offset += retval;
      buff += retval;
      cnt++;
      if (cnt > PREFETCH_MAX_ATTEMPTS)
      {
         clLog()->Error(XrdCl::AppMsg, "Prefetch::DoTask() too many attempts\n %s", m_input.Path());
         m_failed = true;
         retval = -EINTR;
         break;
      }

      if (missing)
      {
         clLog()->Warning(XrdCl::AppMsg, "Prefetch::DoTask() reattempt writing missing %d for block %d %s", missing, task.fileBlockIdx, m_input.Path());
      }
   }

   if (task.condVar)
   {
      XrdSysCondVarHelper(*task.condVar);
      task.condVar->Signal();
   }

   // queue for ram to disk write
   Cache::AddWriteTask(this, task.ramBlockIdx, task.fileBlockIdx, task.size);
}


//_________________________________________________________________________________________________
void 
Prefetch::WriteBlockToDisk(int ramIdx, int fileIdx, size_t size)
{
   // called from XrdFileCache::Cache when process queue

   char* buff = m_ram.m_buffer;
   buff += ramIdx*m_cfi.GetBufferSize();
   int retval = 0;
   // write block buffer into disk file
   {
      long long offset = fileIdx * m_cfi.GetBufferSize();
      int buffer_remaining = size;
      int buffer_offset = 0;
      while ((buffer_remaining > 0) && // There is more to be written
             ((retval = (m_output->Write(&buff[buffer_offset], offset + buffer_offset, buffer_remaining)) != -1)
              || (errno == EINTR))) // Write occurs without an error
      {
         buffer_remaining -= retval;
         buffer_offset += retval;
         if (buffer_remaining)
         {
            clLog()->Warning(XrdCl::AppMsg, "Prefetch::WriteToBlock() reattempt writing missing %d for block %d %s", buffer_remaining, fileIdx, m_input.Path());
         }
      }
   }

   // mark ram block available
   m_ram.m_writeMutex.Lock();
   m_ram.m_blockStates[ramIdx] = 0;
   m_ram.m_writeMutex.UnLock();


   // set downloaded bits
   clLog()->Dump(XrdCl::AppMsg, "Prefetch::WriteToBlock() set bit for block [%d] %s", fileIdx, m_input.Path());
   m_downloadStatusMutex.Lock();
   m_cfi.SetBit(fileIdx);
   m_downloadStatusMutex.UnLock();
}

//______________________________________________________________________________

bool Prefetch::ReadBlockFromTask(int blockIdx, char* buff, long long off, size_t size)
{
   if (Cache::HaveFreeWritingSlots())
   {
      int ramIdx = -1;
      m_ram.m_writeMutex.Lock();
      for (int i =0 ; i < m_ram.m_numBlocks; ++i)  {
         if (m_ram.m_blockStates[i] == 0) {
            ramIdx = i; break;
         }
      }
      m_ram.m_writeMutex.UnLock();

      if (ramIdx >= 0) {
         // create task. check if this is the end block
         size_t taskSize = m_cfi.GetBufferSize();
         int lastFileBlock = (m_input.FSize()-1)/m_cfi.GetBufferSize();
         if (blockIdx == lastFileBlock)
              taskSize = m_input.FSize() - blockIdx*m_cfi.GetBufferSize();
         XrdSysCondVar newTaskCond(0);
         m_tasks_queue.push(Task(blockIdx, ramIdx, taskSize, &newTaskCond));
         XrdSysCondVarHelper xx(newTaskCond);

         newTaskCond.Wait();
         long long inBlockOff = off - blockIdx * m_cfi.GetBufferSize();
         char* srcBuff =  m_ram.m_buffer  + ramIdx*m_cfi.GetBufferSize();
         memcpy(buff, srcBuff - inBlockOff, size);
         return true;
      }
   }
   return false;
}


//______________________________________________________________________________

ssize_t Prefetch::ReadInBlocks(char *buff, off_t off, size_t size)
{

 {
      XrdSysCondVarHelper monitor(m_stateCond);

      if (m_failed) return false;
      
      if ( ! m_started)
      {
         m_stateCond.Wait();
         if (m_failed) return false;
      }
   }

   long long off0 = off;
   int idx_first = off0 / m_cfi.GetBufferSize();
   int idx_last  = (off0 + size -1)/ m_cfi.GetBufferSize();

   size_t bytes_read = 0;
   for (int blockIdx = idx_first; blockIdx <= idx_last; ++blockIdx )
   {
      int readBlockSize = size;
      if (idx_first != idx_last)
      {
         if (blockIdx == idx_first)
         {
            readBlockSize = (blockIdx + 1) * m_cfi.GetBufferSize() - off0;
            clLog()->Debug(XrdCl::AppMsg, "Read partially till the end of the block %", m_input.Path());
         }
         else if (blockIdx == idx_last)
         {
            readBlockSize = (off0+size) - blockIdx*m_cfi.GetBufferSize();
            clLog()->Debug(XrdCl::AppMsg, "Read partially from beginning of block %s", m_input.Path());
         }
         else
         {
            readBlockSize = m_cfi.GetBufferSize();
         }
      }

      int retvalBlock = -1;
      // now do per block read at Read(buff, off, readBlockSize)
      if (m_cfi.TestBit(blockIdx))
      {
         retvalBlock = m_output->Read(buff, off, size);
      }
      else 
      {
         Task task;
         if (ReadBlockFromTask(blockIdx, buff, off, size))
         {
            retvalBlock = size;
         }
         else
         {
            retvalBlock = m_input.Read(buff, off, size);
         }
      }

      if (retvalBlock > 0 )
      {
         bytes_read += retvalBlock;
         buff += retvalBlock;
         off += retvalBlock;

         if ( readBlockSize != retvalBlock)
            return bytes_read;

      }
      else
      {
         return bytes_read;
      }
   }
   return bytes_read;
}



//______________________________________________________________________________
ssize_t 
Prefetch::Read(char *buff, off_t off, size_t size)
{ 
   clLog()->Dump(XrdCl::AppMsg, "Prefetch::Read()  off = %lld size = %lld. %s", off, size, m_input.Path());

   if (m_cfi.IsComplete())
      return m_output->Read(buff, off, size);
   else
      return ReadInBlocks(buff, off, size);
}

//______________________________________________________________________________
void Prefetch::RecordDownloadInfo()
{
   clLog()->Debug(XrdCl::AppMsg, "Prefetch record Info file %s", m_input.Path());
   m_cfi.WriteHeader(m_infoFile);
   m_infoFile->Fsync();
}

//______________________________________________________________________________
void Prefetch::AppendIOStatToFileInfo()
{
   // lock in case several IOs want to write in *cinfo file
   m_downloadStatusMutex.Lock();
   if (m_infoFile)
   {
      m_cfi.AppendIOStat(&m_stats, (XrdOssDF*)m_infoFile);
   }
   else
   {
      clLog()->Warning(XrdCl::AppMsg, "Prefetch::AppendIOStatToFileInfo() info file not opened %s", m_input.Path());
   }
   m_downloadStatusMutex.UnLock();
}
