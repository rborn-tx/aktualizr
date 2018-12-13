#include "fetcher.h"

#ifdef BUILD_OSTREE
#include "package_manager/ostreemanager.h"  // TODO: Hide behind PackageManagerInterface
#endif

namespace Uptane {

bool Fetcher::fetchRole(std::string* result, int64_t maxsize, RepositoryType repo, Uptane::Role role, Version version) {
  // TODO: chain-loading root.json
  std::string base_url = (repo == RepositoryType::Director) ? config.uptane.director_server : config.uptane.repo_server;
  std::string url = base_url + "/" + version.RoleFileName(role);
  HttpResponse response = http->get(url, maxsize);
  if (!response.isOk()) {
    return false;
  }
  *result = response.body;
  return true;
}

static size_t DownloadHandler(char* contents, size_t size, size_t nmemb, void* userp) {
  assert(userp);
  auto* ds = static_cast<Uptane::DownloadMetaStruct*>(userp);
  uint64_t downloaded = size * nmemb;
  uint64_t expected = ds->target.length();
  if ((ds->downloaded_length + downloaded) > expected) {
    return downloaded + 1;  // curl will abort if return unexpected size;
  }

  // incomplete writes will stop the download (written_size != nmemb*size)
  size_t written_size = ds->fhandle->wfeed(reinterpret_cast<uint8_t*>(contents), downloaded);
  ds->hasher().update(reinterpret_cast<const unsigned char*>(contents), written_size);

  ds->downloaded_length += downloaded;
  auto progress = static_cast<unsigned int>((ds->downloaded_length * 100) / expected);
  if (ds->events_channel && progress > ds->last_progress) {
    ds->last_progress = progress;
    auto event = std::make_shared<event::DownloadProgressReport>(ds->target, "Downloading", progress);
    (*(ds->events_channel))(event);
  }
  if (ds->fetcher->isPaused()) {
    ds->fetcher->setRetry(true);
    ds->fhandle->wcommit();
    ds->fhandle.reset();
    return written_size + 1;  // Abort downloading because pause is requested.
  }
  return written_size;
}

PauseResult Fetcher::setPause(bool pause) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (pause_ == pause) {
    if (pause) {
      LOG_INFO << "Download is already paused.";
      sendEvent<event::DownloadPaused>(PauseResult::kAlreadyPaused);
      return PauseResult::kAlreadyPaused;
    } else {
      LOG_INFO << "Download is not paused, can't resume.";
      sendEvent<event::DownloadResumed>(PauseResult::kNotPaused);
      return PauseResult::kNotPaused;
    }
  }

  if (pause && downloading_ == 0u) {
    LOG_INFO << "No download in progress, can't pause.";
    sendEvent<event::DownloadPaused>(PauseResult::kNotDownloading);
    return PauseResult::kNotDownloading;
  }

  pause_ = pause;
  cv_.notify_all();

  if (pause) {
    sendEvent<event::DownloadPaused>(PauseResult::kPaused);
    return PauseResult::kPaused;
  } else {
    sendEvent<event::DownloadResumed>(PauseResult::kResumed);
    return PauseResult::kResumed;
  }
}

void Fetcher::checkPause() {
  std::unique_lock<std::mutex> lk(mutex_);
  cv_.wait(lk, [this] { return !pause_; });
}

bool Fetcher::fetchVerifyTarget(const Target& target) {
  bool result = false;
  DownloadCounter counter(&downloading_);
  try {
    if (!target.IsOstree()) {
      if (target.hashes().empty()) {
        throw Exception("image", "No hash defined for the target");
      }
      auto target_exists = storage->checkTargetFile(target);
      if (target_exists && target_exists->second == target.length()) {
        LOG_INFO << "Image already downloaded skipping download";
        return true;
      }
      DownloadMetaStruct ds(target, events_channel);
      ds.fetcher = this;
      if (!target_exists) {
        ds.fhandle = storage->allocateTargetFile(false, target);
      } else {
        ds.downloaded_length = target_exists->second;
        auto target_handle = storage->openTargetFile(target);
        ds.fhandle = target_handle->toWriteHandle();
        unsigned char buf[ds.downloaded_length];
        target_handle->rread(buf, ds.downloaded_length);
        target_handle->rclose();
        ds.hasher().update(buf, ds.downloaded_length);
      }
      HttpResponse response;
      do {
        checkPause();
        if (retry_) {
          retry_ = false;
          // fhandle was invalidated on pause
          ds.fhandle = storage->openTargetFile(target)->toWriteHandle();
        }
        response = http->download(config.uptane.repo_server + "/targets/" + Utils::urlEncode(target.filename()),
                                  DownloadHandler, &ds, ds.downloaded_length);
        LOG_TRACE << "Download status: " << response.getStatusStr() << std::endl;
      } while (retry_);
      if (!response.isOk()) {
        if (response.curl_code == CURLE_WRITE_ERROR) {
          throw OversizedTarget(target.filename());
        }
        throw Exception("image", "Could not download file, error: " + response.error_message);
      }
      if (!target.MatchWith(Hash(ds.hash_type, ds.hasher().getHexDigest()))) {
        ds.fhandle->wabort();
        throw TargetHashMismatch(target.filename());
      }
      ds.fhandle->wcommit();
      result = true;
    } else {
#ifdef BUILD_OSTREE
      KeyManager keys(storage, config.keymanagerConfig());
      keys.loadKeys();
      std::function<void()> pause = std::bind(&Fetcher::checkPause, this);
      data::InstallOutcome outcome =
          OstreeManager::pull(config.pacman.sysroot, config.pacman.ostree_server, keys, target, pause, events_channel);
      result =
          (outcome.first == data::UpdateResultCode::kOk || outcome.first == data::UpdateResultCode::kAlreadyProcessed);
#else
      LOG_ERROR << "Could not pull OSTree target. Aktualizr was built without OSTree support!";
#endif
    }
  } catch (const Exception& e) {
    LOG_WARNING << "Error while downloading a target: " << e.what();
  }
  return result;
}

}  // namespace Uptane
