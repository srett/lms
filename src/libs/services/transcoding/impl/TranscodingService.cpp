/*
 * Copyright (C) 2025 Emeric Poupon
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

#include "TranscodingService.hpp"

#include "av/ITranscoder.hpp"
#include "core/ILogger.hpp"
#include "database/IDb.hpp"
#include "database/Session.hpp"
#include "database/objects/Track.hpp"

#include "TranscodingResourceHandler.hpp"
#include "core/IConfig.hpp"

namespace lms::transcoding
{
    namespace
    {
        av::OutputParameters toAv(const OutputParameters& out)
        {
            return { .format = static_cast<lms::av::OutputFormat>(out.format), .bitrate = out.bitrate, .stripMetadata = out.stripMetadata };
        }

        std::size_t doEstimateContentLength(std::size_t bitrate, std::chrono::milliseconds duration)
        {
            const std::size_t estimatedContentLength{ static_cast<size_t>((bitrate / 8 * duration.count()) / 1000) };
            return estimatedContentLength;
        }
    } // namespace

    std::unique_ptr<ITranscodingService> createTranscodingService(db::IDb& db, core::IChildProcessManager& childProcessManager, boost::asio::io_context& ioContext)
    {
        auto* config = core::Service<core::IConfig>::get();
        auto size = config->getULong("transcode-cache-size", 0);
        bool useCaching{ false };

        LMS_LOG(TRANSCODING, INFO, "Configured transcoding cache size: " << size << "MiB");
        if (size > 0)
        {
            // TODO: Should be size of cache in MB, auto-clean (LRU), write worker
            const std::filesystem::path cachePath{ config->getPath("working-dir", "/var/lms") / "cache" / "transcode" };
            std::error_code ec;
            std::filesystem::create_directories(cachePath, ec);
            if (!ec)
                LMS_LOG(TRANSCODING, WARNING, "Creating " << cachePath << " failed, disabling");
            else
                useCaching = true;
        }

        return std::make_unique<TranscodingService>(db, childProcessManager, ioContext, useCaching);

    }

    TranscodingService::TranscodingService(db::Db& db, core::IChildProcessManager& childProcessManager, boost::asio::io_context& ioContext, bool useCaching)
        : _db{ db }
        , _childProcessManager(childProcessManager)
        , _ioContext{ ioContext }
        , _useCaching{ useCaching }
    {
        LMS_LOG(TRANSCODING, INFO, "Service started!");
    }

    TranscodingService::~TranscodingService()
    {
        LMS_LOG(TRANSCODING, INFO, "Service stopped!");
    }

    std::unique_ptr<core::IResourceHandler> TranscodingService::createResourceHandler(const InputParameters& inputParameters, const OutputParameters& outputParameters, bool estimateContentLength)
    {
        av::InputParameters avInputParams;
        std::optional<std::size_t> estimatedContentLength;
        {
            auto& session{ _db.getTLSSession() };
            auto transaction{ session.createReadTransaction() };

            db::Track::pointer track{ db::Track::find(session, inputParameters.trackId) };
            if (!track)
                return nullptr;

            avInputParams.file = track->getAbsoluteFilePath();
            avInputParams.offset = inputParameters.offset;
            avInputParams.streamIndex = inputParameters.streamIndex;

            if (estimateContentLength)
                estimatedContentLength = doEstimateContentLength(outputParameters.bitrate, track->getDuration());
        }

        return std::make_unique<TranscodingResourceHandler>(avInputParams, toAv(outputParameters), estimatedContentLength);
    }
} // namespace lms::transcoding
