/*
* Copyright (C) 2025 Simon Rettberg
*
* This file is part of LMS.
*
* LMS is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* LMS is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with LMS.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <utility> // XXX: Fixes compile on Debian 12 for me (See #681)

#include "CachingTranscoderSession.hpp"
#include "CachingTranscoderClientHandler.hpp"
#include "TranscodingResourceHandler.hpp"
#include "core/FileResourceHandlerCreator.hpp"
#include "core/IConfig.hpp"
#include "core/ILogger.hpp"

namespace lms::transcoding
{
   namespace
   {
       // TODO: Configurable? E.g. allowed-transcoding-bitrates = "32 64 128 ...."
       const std::vector<std::size_t> ALLOWED_BITRATES{320'000, 256'000, 192'000, 160'000, 128'000, 96'000, 64'000, 32'000};

       std::atomic<size_t> instCount{}; // XXX: Just during development to find memory leaks through dangling/cyclic references

       std::unordered_map<uint64_t, std::shared_ptr<CachingTranscoderSession>> jobs{};
       std::mutex jobMutex{};

       std::filesystem::path getCacheFile(const std::filesystem::path &cachePath, uint64_t hash)
       {
           std::ostringstream oss;
           oss << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << hash;
           auto subDir = cachePath / oss.str().substr(0, 1); // Or 2 chars? How many files will users cache? Avoid thousands of files in one directory
           std::error_code ec;
           std::filesystem::create_directories(subDir, ec);
           if (ec)
           {
               LMS_LOG(TRANSCODING, WARNING, "Error creating cache sub-directory: " << ec.message());
               return {};
           }
           return subDir / oss.str();
       }

       void removeJobFromMap(CachingTranscoderSession* sess)
       {
           std::lock_guard<std::mutex> const guard{ jobMutex };
           sess->renameTmpFile();
           if (!jobs.erase(sess->hash()))
               LMS_LOG(TRANSCODING, DEBUG, "remove Job: Not found!");
       }
   } // namespace

   std::shared_ptr<core::IResourceHandler> createCachingTranscodingResourceHandler(av::InputParameters inputParameters, av::OutputParameters outputParametersOriginal, const std::optional<size_t>& estimatedContentLength, boost::asio::io_context& ioContext, const std::filesystem::path& cachePath)
   {
       assert(inputParameters.offset.count() == 0);
       // Snap to predefined values, to increase chances of cache hit
       auto outputParameters{ outputParametersOriginal };
       for (auto rate : ALLOWED_BITRATES)
       {
           if (outputParameters.bitrate >= rate)
           {
               outputParameters.bitrate = rate;
               break;
           }
       }
       // Enforce minimum
       outputParameters.bitrate = std::max(outputParameters.bitrate, ALLOWED_BITRATES.back());
       if (outputParametersOriginal.bitrate != outputParameters.bitrate)
           LMS_LOG(TRANSCODING, DEBUG, "Bitrate forced from " << outputParametersOriginal.bitrate / 1000 << " to " << outputParameters.bitrate / 1000);
       // Lookup in cache
       auto hash = inputParameters.hash() ^ outputParameters.hash();
       const std::lock_guard<std::mutex> guard{ jobMutex };
       auto it = jobs.find(hash);
       std::shared_ptr<CachingTranscoderSession> job{};
       if (it != jobs.end())
       {
           // Ongoing transcode, attach
           LMS_LOG(TRANSCODING, DEBUG, "Existing transcode job, attaching");
           job = it->second;
       }
       else
       {
           // No active job, check cache
           auto filePath = getCacheFile(cachePath, hash);
           std::error_code ec;
           if (!filePath.empty() && is_regular_file(filePath, ec))
           {
               LMS_LOG(TRANSCODING, DEBUG, "Transcoded file already in cache");
               // touch file to mark last use (a bit ugly, but otherwise we need to keep track of last use separately)
               // TODO: Worker that deletes old files if cache is too large - scan maybe once an hour/day
               ec.clear();
               std::filesystem::last_write_time(filePath, std::filesystem::file_time_type::clock::now(), ec);
               if (ec)
                   LMS_LOG(TRANSCODING, WARNING, "Cannot update timestamp of cached file '" << filePath << ": " << ec.message());
               auto handler = core::createFileResourceHandler(filePath, outputParameters.formatToMimeType());
               //if (handler->sourceGood()) // File is readable, return raw handler
                   return handler;
               LMS_LOG(TRANSCODING, WARNING, "File access error, transcoding file again");
           }
           // TODO: Maybe check if there's enough space left on disk, and fall back to the old transcoder otherwise
           LMS_LOG(TRANSCODING, DEBUG, "Creating shared caching transcoder");
           job = std::make_shared<CachingTranscoderSession>(hash, filePath, inputParameters, outputParameters);
           jobs.emplace(hash, job);
       }
       if (job)
           return job->newClient(estimatedContentLength, ioContext);
       // Something went wrong; fall back to plain old transcoding handler
       LMS_LOG(TRANSCODING, INFO, "Falling back to simple transcoder");
       return {};
   }

   std::shared_ptr<core::IResourceHandler> CachingTranscoderSession::newClient(const std::optional<size_t>& estimatedContentLength, boost::asio::io_context& ioContext)
   {
       std::shared_ptr<CachingTranscoderClientHandler> ptr{ std::make_shared<CachingTranscoderClientHandler>(shared_from_this(), estimatedContentLength, ioContext) };
       {
           std::lock_guard<std::mutex> const guard{ _clientMutex };
           _clients.emplace_back(ptr);
       }
       ptr->update(_currentFileLength, CachingTranscoderClientHandler::WORKING);
       return ptr;
   }

   CachingTranscoderSession::CachingTranscoderSession(uint64_t hash, const std::filesystem::path &file, const av::InputParameters& inputParameters, const av::OutputParameters& outputParameters)
       : _destFile{ file }
       , _fs{ std::filesystem::path(file) += ".tmp", std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc }
       , _transcoder{ av::createTranscoder(inputParameters, outputParameters) }
       , _jobHash{ hash }
   {
       LMS_LOG(TRANSCODING, DEBUG, "CachingTranscoderSession instances: " << ++instCount);
       keepReading();
   }

   CachingTranscoderSession::~CachingTranscoderSession()
   {
       LMS_LOG(TRANSCODING, DEBUG, "CachingTranscoderSession instances: " << --instCount);
   }

   int64_t CachingTranscoderSession::serveBytes(std::ostream& stream, uint64_t offset, int64_t len)
   {
       if (offset >= _currentFileLength || len <= 0)
           return 0;
       std::int64_t bytesRead;
       bool good;
       std::vector<char> buffer(std::min<std::size_t>(len, CHUNK_SIZE));
       {
           std::lock_guard<std::mutex> const guard{ _fsMutex };
           _fs.seekg(offset);
           _fs.read(buffer.data(), buffer.size());
           bytesRead = _fs.gcount();
           good = _fs.good();
       }
       if (!good)
       {
           LMS_LOG(TRANSCODING, WARNING, "Error on cached file fstream while serving a client");
           return -1;
       }
       stream.write(buffer.data(), bytesRead);
       return bytesRead;
   }

   void CachingTranscoderSession::keepReading()
   {
       LMS_LOG(TRANSCODING, DEBUG, "keepReading");
       if (_transcoder->finished())
       {
           LMS_LOG(TRANSCODING, DEBUG, "Caching transcoder job finished, bytes produced: " << _currentFileLength << ", clients left: " << _clients.size());
           notifyClients(CachingTranscoderClientHandler::DONE);
           removeJobFromMap(this);
           return;
       }

       _transcoder->asyncRead(_buffer.data(), _buffer.size(), [this](std::size_t nbBytesRead) {
           LMS_LOG(TRANSCODING, DEBUG, "Have " << nbBytesRead << " more bytes to send back and cache");
           if (nbBytesRead)
           {
               bool good;
               {
                   std::lock_guard<std::mutex> const guard{ _fsMutex };
                   _fs.seekp(_currentFileLength, std::ios::beg);
                   _fs.write(reinterpret_cast<const char*>(_buffer.data()), nbBytesRead);
                   _fs.flush();
                   good = _fs.good();
               }
               if (!good)
               {
                   LMS_LOG(TRANSCODING, WARNING, "Error writing to transcoded cache file");
                   notifyClients(CachingTranscoderClientHandler::ERROR);
                   return;
               }
           }
           _currentFileLength += nbBytesRead;
           notifyClients(CachingTranscoderClientHandler::WORKING);
           this->keepReading();
       });
   }

   void CachingTranscoderSession::notifyClients(CachingTranscoderClientHandler::UpdateStatus status)
   {
       int done{};
       std::size_t remaining;
       std::lock_guard<std::mutex> const guard{ _clientMutex };

       if (status == CachingTranscoderClientHandler::WORKING)
       {
           for (int i = static_cast<int>(_clients.size()) - 1; i >= 0; --i)
           {
               if (!_clients[i]->update(_currentFileLength, status))
               {
                   done++;
                   _clients[i] = std::move(_clients.back());
                   _clients.pop_back();
               }
           }
           remaining = _clients.size();
           LMS_LOG(TRANSCODING, DEBUG, "Clients done with shared transcoder: " << done << ", remaining: " << remaining);
       }
       else
       {
           for (const auto& client : _clients)
               client->update(_currentFileLength, status);
           _clients.clear();
       }
   }

   void CachingTranscoderSession::renameTmpFile()
   {
       std::error_code ec;
       std::filesystem::rename((std::filesystem::path(_destFile) += ".tmp"), _destFile, ec);
       if (ec) {
           LMS_LOG(TRANSCODING, DEBUG, "Cannot rename transcoded file " << _destFile << ": " << ec.message());
       }
   }

} // namespace lms::transcoding