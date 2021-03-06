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

#include "torrent-manager.hpp"
#include "file-manifest.hpp"

#include "torrent-file.hpp"
#include "util/io-util.hpp"
#include "util/logging.hpp"

#include <boost/asio/io_service.hpp>
#include <boost/filesystem.hpp>

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/io.hpp>

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = boost::filesystem;

using std::string;
using std::vector;

namespace ndn {
namespace ntorrent {

static vector<TorrentFile>
intializeTorrentSegments(const string& torrentFilePath, const Name& initialSegmentName)
{
  security::KeyChain key_chain;
  Name currSegmentFullName = initialSegmentName;
  vector<TorrentFile> torrentSegments = IoUtil::load_directory<TorrentFile>(torrentFilePath);
  // Starting with the initial segment name, verify the names, loading next name from torrentSegment
  for (auto it = torrentSegments.begin(); it != torrentSegments.end(); ++it) {
    TorrentFile& segment = *it;
    key_chain.sign(segment, signingWithSha256());
    if (segment.getFullName() != currSegmentFullName) {
      vector<TorrentFile> correctSegments(torrentSegments.begin(), it);
      torrentSegments.swap(correctSegments);
      break;
    }
    // load the next full name
    if (nullptr == segment.getTorrentFilePtr()) {
      break;
    }
    currSegmentFullName = *segment.getTorrentFilePtr();
  }
  return torrentSegments;
}

static vector<FileManifest>
intializeFileManifests(const string& manifestPath, const vector<TorrentFile>& torrentSegments)
{
  security::KeyChain key_chain;

  vector<FileManifest> manifests = IoUtil::load_directory<FileManifest>(manifestPath);
  if (manifests.empty()) {
    return manifests;
  }

  // sign the manifests
  std::for_each(manifests.begin(), manifests.end(),
                [&key_chain](FileManifest& m){
                  key_chain.sign(m,signingWithSha256());
                });

  // put all names of initial manifests from the valid torrent files into a set
  std::vector<ndn::Name> validInitialManifestNames;
  for (const auto& segment : torrentSegments) {
    const auto& catalog = segment.getCatalog();
    validInitialManifestNames.insert(validInitialManifestNames.end(),
                                    catalog.begin(),
                                    catalog.end());
  }
  auto manifest_it =  manifests.begin();
  std::vector<FileManifest> output;
  output.reserve(manifests.size());

  for (auto& initialName : validInitialManifestNames) {
    // starting from the initial segment
    auto& validName = initialName;
    if (manifests.end() == manifest_it) {
      break;
    }
    auto fileName = manifest_it->file_name();
    // sequential collect all valid segments
    while (manifest_it != manifests.end() && manifest_it->getFullName() == validName) {
      output.push_back(*manifest_it);
      if (manifest_it->submanifest_ptr() != nullptr) {
        validName = *manifest_it->submanifest_ptr();
        ++manifest_it;
      }
      else {
        ++manifest_it;
        break;
      }
    }
    // skip the remain segments for this file (all invalid)
    while (manifests.end() != manifest_it && manifest_it->file_name() == fileName) {
      ++manifest_it;
    }
  }
  return output;
}

static vector<Data>
initializeDataPackets(const string&      filePath,
                      const FileManifest manifest,
                      size_t             subManifestSize)
{
  vector<Data> packets;
  auto subManifestNum = manifest.submanifest_number();

  packets =  IoUtil::packetize_file(filePath,
                                    manifest.name(),
                                    manifest.data_packet_size(),
                                    subManifestSize,
                                    subManifestNum);

  auto catalog = manifest.catalog();
  // Filter out invalid packet names
  std::remove_if(packets.begin(), packets.end(),
                [&packets, &catalog](const Data& p) {
                  return catalog.end() == std::find(catalog.begin(),
                                                     catalog.end(),
                                                     p.getFullName());
                });
  return packets;
}

static std::vector<bool>
initializeFileState(const string&       dataPath,
                    const FileManifest& manifest,
                    size_t              subManifestSize)
{
  // construct the file name
  return vector<bool>(manifest.catalog().size());
}

//==================================================================================================
//                                    TorrentManager Implementation
//==================================================================================================

void TorrentManager::Initialize()
{
  // initialize the update handler

  // figure out the name of the torrent
  Name torrentName;
  Name scheme(SharedConstants::commonPrefix);
  if (m_torrentFileName.get(m_torrentFileName.size() - 2).isSequenceNumber()) {
    torrentName = m_torrentFileName.getSubName(1 + scheme.size(), m_torrentFileName.size() - (4 + scheme.size()));
  }
  else {
    torrentName = m_torrentFileName.getSubName(1 + scheme.size(), m_torrentFileName.size() - (3 + scheme.size()));
  }

  m_updateHandler = make_shared<UpdateHandler>(torrentName, m_keyChain,
                                               make_shared<StatsTable>(m_statsTable), m_face,
                                               std::bind(&TorrentManager::eraseOwnRoutablePrefix,
                                                         this));

  // .../<torrent_name>/torrent-file/<implicit_digest>
  string dataPath = ".appdata/" + m_torrentFileName.get(-3).toUri();
  string manifestPath = dataPath +"/manifests";
  string torrentFilePath = dataPath +"/torrent_files";

  // get the torrent file segments and manifests that we have.
  if (!Io::exists(torrentFilePath)) {
    return;
  }
  m_torrentSegments = intializeTorrentSegments(torrentFilePath, m_torrentFileName);
  if (m_torrentSegments.empty()) {
    return;
  }
  m_fileManifests   = intializeFileManifests(manifestPath, m_torrentSegments);

  // get the submanifest sizes
  for (const auto& m : m_fileManifests) {
    if (m.submanifest_number() == 0) {
      auto manifestFileName = m.file_name();
      m_subManifestSizes[manifestFileName] = m.catalog().size();
    }
  }

  for (const auto& m : m_fileManifests) {
    // construct the file name
    auto fileName = m.file_name();
    fs::path filePath = m_dataPath + fileName;

    if (!Io::exists(filePath)) {
      if (!Io::exists(filePath.parent_path())) {
        IoUtil::create_directories(filePath.parent_path());
      }
      continue;
    }
    auto packets = initializeDataPackets(filePath.string(), m, m_subManifestSizes[m.file_name()]);
    // If there are any valid packets, add corresponding state to manager
    if (!packets.empty()) {
      m_fileStates[m.getFullName()] = initializeFileState(m_dataPath,
                                                          m,
                                                          m_subManifestSizes[m.file_name()]);
      auto& fileBitMap = m_fileStates[m.getFullName()];
      auto read_it = packets.begin();
      size_t i = 0;
      for (auto name : m.catalog()) {
        if (read_it == packets.end()) {
          break;
        }
        if (name == read_it->getFullName()) {
          ++read_it;
          fileBitMap[i] = true;
        }
        ++i;
      }
      for (const auto& d : packets) {
        seed(d);
      }
    }
  }
  for (const auto& t : m_torrentSegments) {
    seed(t);
  }
  for (const auto& m : m_fileManifests) {
   seed(m);
  }
}

shared_ptr<Name>
TorrentManager::findTorrentFileSegmentToDownload() const
{
  // if we have no segments
  if (m_torrentSegments.empty()) {
    return make_shared<Name>(m_torrentFileName);
  }
  // otherwise just return the next segment ptr of the last segment we have
  return m_torrentSegments.back().getTorrentFilePtr();
}

shared_ptr<Name>
TorrentManager::findManifestSegmentToDownload(const Name& manifestName) const
{
  //sequentially find whether we have downloaded any segments of this manifest file
  Name manifestPrefix = FileManifest::manifestPrefix(manifestName);
  auto it = std::find_if(m_fileManifests.rbegin(), m_fileManifests.rend(),
                      [&manifestPrefix] (const FileManifest& f) {
                        return manifestPrefix.isPrefixOf(f.getName());
                      });

  // if we do not have any segments of the file manifest
  if (it == m_fileManifests.rend()) {
    return make_shared<Name>(manifestName);
  }

  // if we already have the requested segment of the file manifest
  if (it->submanifest_number() >= manifestName.get(manifestName.size() - 2).toSequenceNumber()) {
    return it->submanifest_ptr();
  }
  // if we do not have the requested segment
  else {
    return make_shared<Name>(manifestName);
  }
}

void
TorrentManager::findFileManifestsToDownload(std::vector<Name>& manifestNames) const
{
  std::vector<Name> manifests;
  // insert the first segment name of all the file manifests to the vector
  for (auto i = m_torrentSegments.begin(); i != m_torrentSegments.end(); i++) {
    manifests.insert(manifests.end(), i->getCatalog().begin(), i->getCatalog().end());
  }
  // for each file
  for (const auto& manifestName : manifests) {
    // find the first (if any) segment we are missing
    shared_ptr<Name> manifestSegmentName = findManifestSegmentToDownload(manifestName);
    if (nullptr != manifestSegmentName) {
      manifestNames.push_back(*manifestSegmentName);
    }
  }
}

bool
TorrentManager::hasDataPacket(const Name& dataName) const
{

  auto manifest_it = std::find_if(m_fileManifests.begin(), m_fileManifests.end(),
                                 [&dataName](const FileManifest& m) {
                                   return m.getName().isPrefixOf(dataName);
                                 });

  // if we do not have the file manifest, just return false
  if (manifest_it == m_fileManifests.end()) {
    return false;
  }

  // that corresponds to the specific submanifest
  auto fileState_it = m_fileStates.find(manifest_it->getFullName());
  if (m_fileStates.end() != fileState_it) {
    const auto& fileState = fileState_it->second;
    auto dataNum = dataName.get(dataName.size() - 2).toSequenceNumber();
    // find whether we have the requested packet from the bitmap
    return fileState[dataNum];
  }
  return false;
}

void
TorrentManager::findDataPacketsToDownload(const Name& manifestName, std::vector<Name>& packetNames) const
{
  auto manifest_it = std::find_if(m_fileManifests.begin(), m_fileManifests.end(),
                                 [&manifestName](const FileManifest& m) {
                                   return m.name().getSubName(0, m.name().size()
                                                              - 1).isPrefixOf(manifestName);
                                 });

  for (auto j = manifest_it; j != m_fileManifests.end(); j++) {
    auto& fileState = m_fileStates[j->getFullName()];
    for (size_t dataNum = 0; dataNum < j->catalog().size(); ++dataNum) {
      if (!fileState[dataNum]) {
        packetNames.push_back(j->catalog()[dataNum]);
      }
    }

    // check that the next manifest in the vector refers to the next segment of the same file
    if ((j + 1) != m_fileManifests.end() && (j+1)->file_name() != manifest_it->file_name()) {
      break;
    }
  }
}

void
TorrentManager::findAllMissingDataPackets(std::vector<Name>& packetNames) const
{
  for (auto j = m_fileManifests.begin(); j != m_fileManifests.end(); ++j) {
    auto fileState_it = m_fileStates.find(j->getFullName());
    // if we have no packets from this file
    if (m_fileStates.end() == fileState_it) {
      packetNames.reserve(packetNames.size() + j->catalog().size());
      packetNames.insert(packetNames.end(), j->catalog().begin(), j->catalog().end());
    }
    // find the packets that we are missing
    else {
      const auto &fileState =  fileState_it->second;
      for (auto i = j->catalog().begin(); i != j->catalog().end(); i++) {
        auto dataNum = i->get(i->size() - 2).toSequenceNumber();
        if (!fileState[dataNum]) {
          packetNames.push_back(*i);
        }
      }
    }
  }
}

void
TorrentManager::downloadTorrentFileSegment(const ndn::Name& name,
                                           const std::string& path,
                                           TorrentFileReceivedCallback onSuccess,
                                           FailedCallback onFailed)
{
  shared_ptr<Interest> interest = createInterest(name);

  auto dataReceived = [path, onSuccess, onFailed, this]
                                            (const Interest& interest, const Data& data) {
      m_pendingInterests.erase(interest.getName());
      // Stats Table update here...
      m_stats_table_iter->incrementReceivedData();
      m_retries = 0;
      std::vector<Name> manifestNames;
      TorrentFile file(data.wireEncode());

      // Write the torrent file segment to disk...
      if (writeTorrentSegment(file, path)) {
        // if successfully written, seed this data
        seed(file);
      }
      const std::vector<Name>& manifestCatalog = file.getCatalog();
      manifestNames.insert(manifestNames.end(), manifestCatalog.begin(), manifestCatalog.end());

      shared_ptr<Name> nextSegmentPtr = file.getTorrentFilePtr();
      if (onSuccess) {
        onSuccess(manifestNames);
      }
      if (nextSegmentPtr != nullptr) {
        this->downloadTorrentFileSegment(*nextSegmentPtr, path, onSuccess, onFailed);
      }
      this->sendInterest();
      if (!hasPendingInterests() && !m_seedFlag) {
        shutdown();
      }
  };

  auto dataFailed = [path, name, onSuccess, onFailed, this]
                                                (const Interest& interest) {
    m_pendingInterests.erase(interest.getName());
    ++m_retries;
    if (m_retries >= MAX_NUM_OF_RETRIES) {
      ++m_stats_table_iter;
      if (m_stats_table_iter == m_statsTable.end()) {
        m_stats_table_iter = m_statsTable.begin();
      }
    }
    if (onFailed) {
      onFailed(interest.getName(), "Unknown error");
    }
    this->sendInterest();
    if (!hasPendingInterests() && !m_seedFlag) {
      shutdown();
    }
  };
  LOG_DEBUG << "Pushing to the Interest Queue: " << *interest << std::endl;
  m_interestQueue->push(interest, dataReceived, dataFailed);
  this->sendInterest();
}

void
TorrentManager::downloadTorrentFile(const std::string& path,
                                    TorrentFileReceivedCallback onSuccess,
                                    FailedCallback onFailed)
{
  shared_ptr<Name> searchRes = this->findTorrentFileSegmentToDownload();
  auto manifestNames = make_shared<std::vector<Name>>();
  if (m_updateHandler->needsUpdate() && !(m_updateHandler->getOwnRoutablePrefix().empty())) {
    //m_updateHandler->sendAliveInterest(m_stats_table_iter);
  }
  if (searchRes != nullptr) {
    this->downloadTorrentFileSegment(*searchRes, path, onSuccess, onFailed);
  }
  else {
    std::vector<Name> manifests;
    findFileManifestsToDownload(manifests);
    if (onSuccess) {
      onSuccess(manifests);
    }
  }
}

void
TorrentManager::download_file_manifest(const Name&              manifestName,
                                       const std::string&       path,
                                       TorrentManager::ManifestReceivedCallback onSuccess,
                                       TorrentManager::FailedCallback           onFailed)
{
  shared_ptr<Name> searchRes = findManifestSegmentToDownload(manifestName);
  auto packetNames = make_shared<std::vector<Name>>();
  if (m_updateHandler->needsUpdate() && !(m_updateHandler->getOwnRoutablePrefix().empty())) {
    m_updateHandler->sendAliveInterest(m_stats_table_iter);
  }
  if (searchRes == nullptr) {
    this->findDataPacketsToDownload(manifestName, *packetNames);
    onSuccess(*packetNames);
    return;
  }
  this->downloadFileManifestSegment(*searchRes, path, packetNames, onSuccess, onFailed);
}

void
TorrentManager::download_data_packet(const Name& packetName,
                                     DataReceivedCallback onSuccess,
                                     FailedCallback onFailed)
{
  if (this->hasDataPacket(packetName)) {
    onSuccess(packetName);
    return;
  }

  shared_ptr<Interest> interest = this->createInterest(packetName);

  auto dataReceived = [onSuccess, onFailed, this]
                                          (const Interest& interest, const Data& data) {
    m_pendingInterests.erase(interest.getName());
    // Write data to disk...
    if(writeData(data)) {
      seed(data);
    }
    // Stats Table update here...
    m_stats_table_iter->incrementReceivedData();
    m_retries = 0;
    onSuccess(data.getName());
    this->sendInterest();
    if (!hasPendingInterests() && !m_seedFlag) {
      shutdown();
    }
  };

  auto dataFailed = [onFailed, this]
                             (const Interest& interest) {
    m_retries++;
    m_pendingInterests.erase(interest.getName());
    if (m_retries >= MAX_NUM_OF_RETRIES) {
      m_stats_table_iter++;
      if (m_stats_table_iter == m_statsTable.end()) {
        m_stats_table_iter = m_statsTable.begin();
      }
    }
    onFailed(interest.getName(), "Unknown failure");
    this->sendInterest();
    if (!hasPendingInterests() && !m_seedFlag) {
      shutdown();
    }
  };
  LOG_DEBUG << "Pushing to the Interest Queue: " << *interest << std::endl;
  m_interestQueue->push(interest, dataReceived, dataFailed);
  this->sendInterest();
}

void TorrentManager::seed(const Data& data) {
  auto dataName = data.getFullName();
  std::shared_ptr<Name> prefix = nullptr;

  switch (IoUtil::findType(dataName)) {
    case IoUtil::NAME_TYPE::TORRENT_FILE: {
      if (hasAllTorrentSegments()) {
        prefix = std::make_shared<Name>(TorrentFile::torrentFileName(dataName));
      }
    } break;
    case IoUtil::NAME_TYPE::FILE_MANIFEST: {
      if (hasAllManifestSegments(dataName)) {
        prefix = std::make_shared<Name>(FileManifest::manifestPrefix(dataName));
      }
    } break;
    case IoUtil::NAME_TYPE::DATA_PACKET: /* we  already announced the prefix */
    case IoUtil::NAME_TYPE::UNKNOWN: {
    } break;
    default:
      break;
  }
  if (prefix) {
    m_face->setInterestFilter(*prefix,
                             bind(&TorrentManager::onInterestReceived, this, _1, _2),
                             RegisterPrefixSuccessCallback(),
                             bind(&TorrentManager::onRegisterFailed, this, _1, _2));
  }
}

void
TorrentManager::shutdown()
{
  m_face->getIoService().stop();
}

// = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
//                                Protected Helpers
// = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =

bool TorrentManager::writeData(const Data& packet)
{
  // find correct manifest
  const auto& packetName = packet.getName();
  auto manifest_it = std::find_if(m_fileManifests.begin(), m_fileManifests.end(),
                                 [&packetName](const FileManifest& m) {
                                   return m.getName().isPrefixOf(packetName);
                                 });
  if (m_fileManifests.end() == manifest_it) {
    return false;
  }
  // get file state out
  auto fileState_it = m_fileStates.find(manifest_it->getFullName());
  // if there is no open stream to the file
  if(fileState_it == m_fileStates.end()) {
    fs::path filePath = m_dataPath + manifest_it->file_name();
    if (!Io::exists(filePath)) {
      IoUtil::create_directories(filePath.parent_path());
    }
    m_fileStates[manifest_it->getFullName()] =
                initializeFileState(m_dataPath,
                                    *manifest_it,
                                    m_subManifestSizes[manifest_it->file_name()]);
  }
  auto& fileState = m_fileStates[manifest_it->getFullName()];
  auto packetNum = packetName.get(packetName.size() - 1).toSequenceNumber();
  // if we already have the packet, do not rewrite it.
  if (fileState[packetNum]) {
    return false;
  }
  // write data to disk
  auto subManifestSize = m_subManifestSizes[manifest_it->file_name()];
  auto filePath = m_dataPath + manifest_it->file_name();
  if (IoUtil::writeData(packet, *manifest_it, subManifestSize, filePath)) {;
    // update bitmap
    fileState[packetNum] = true;
    return true;
  }
  LOG_ERROR << "Write failed: " << packet.getFullName() << std::endl;
  return false;
}

bool
TorrentManager::writeTorrentSegment(const TorrentFile& segment, const std::string& path)
{
  // validate  the torrent
  auto torrentPrefix = m_torrentFileName.getSubName(0, m_torrentFileName.size() - 1);
  // check if we already have it
  if (torrentPrefix.isPrefixOf(segment.getName()) &&
      m_torrentSegments.end() == std::find(m_torrentSegments.begin(), m_torrentSegments.end(),
                                           segment))
  {
    if(IoUtil::writeTorrentSegment(segment, path)) {
      auto it = std::find_if(m_torrentSegments.begin(), m_torrentSegments.end(),
                             [&segment](const TorrentFile& t){
                               return segment.getSegmentNumber() < t.getSegmentNumber() ;
                            });
      m_torrentSegments.insert(it, segment);
      return true;
    }
  }
  return false;
}


bool TorrentManager::writeFileManifest(const FileManifest& manifest, const std::string& path)
{
  if (m_fileManifests.end() == std::find(m_fileManifests.begin(), m_fileManifests.end(),
                                           manifest))
  {
    // update the state of the manager
    if (0 == manifest.submanifest_number()) {
      m_subManifestSizes[manifest.file_name()] = manifest.catalog().size();
    }
    if(IoUtil::writeFileManifest(manifest, path)) {
      // add to collection
      auto it = std::find_if(m_fileManifests.begin(), m_fileManifests.end(),
                             [&manifest](const FileManifest& m){
                               return m.file_name() >  manifest.file_name()
                               ||    (m.file_name() == manifest.file_name()
                                  && (m.submanifest_number() > manifest.submanifest_number()));
                            });
      m_fileManifests.insert(it, manifest);
      return true;
    }
  }
  return false;
}

void
TorrentManager::downloadFileManifestSegment(const Name& manifestName,
                                            const std::string& path,
                                            std::shared_ptr<std::vector<Name>> packetNames,
                                            TorrentManager::ManifestReceivedCallback onSuccess,
                                            TorrentManager::FailedCallback onFailed)
{
  shared_ptr<Interest> interest = this->createInterest(manifestName);

  auto dataReceived = [packetNames, path, onSuccess, onFailed, this]
                                          (const Interest& interest, const Data& data) {
    m_pendingInterests.erase(interest.getName());
    // Stats Table update here...
    m_stats_table_iter->incrementReceivedData();
    m_retries = 0;

    FileManifest file(data.wireEncode());

    // Write the file manifest segment to disk...
    if(writeFileManifest(file, path)) {
      seed(file);
    }
    else {
      onFailed(interest.getName(), "Write Failed");
    }

    const std::vector<Name>& packetsCatalog = file.catalog();
    packetNames->insert(packetNames->end(), packetsCatalog.begin(), packetsCatalog.end());
    shared_ptr<Name> nextSegmentPtr = file.submanifest_ptr();
    if (nextSegmentPtr != nullptr) {
      this->downloadFileManifestSegment(*nextSegmentPtr, path, packetNames, onSuccess, onFailed);
    }
    else {
      onSuccess(*packetNames);
    }
    this->sendInterest();
    if (!hasPendingInterests() && !m_seedFlag) {
      shutdown();
    }
  };

  auto dataFailed = [packetNames, path, manifestName, onFailed, this]
                                                (const Interest& interest) {
    m_pendingInterests.erase(interest.getName());
    m_retries++;
    if (m_retries >= MAX_NUM_OF_RETRIES) {
      m_stats_table_iter++;
      if (m_stats_table_iter == m_statsTable.end())
        m_stats_table_iter = m_statsTable.begin();
    }
    onFailed(interest.getName(), "Unknown failure");
    this->sendInterest();
  };
  LOG_DEBUG << "Pushing to the Interest Queue: " << *interest << std::endl;
  m_interestQueue->push(interest, dataReceived, dataFailed);
  this->sendInterest();
}

void
TorrentManager::onInterestReceived(const InterestFilter& filter, const Interest& interest)
{
  // handle if it is a torrent-file
  LOG_DEBUG << "Interest Received: " << interest << std::endl;
  const auto& interestName = interest.getName();
  std::shared_ptr<Data> data = nullptr;
  auto cmp = [&interestName](const Data& t){return t.getFullName() == interestName;};
  // determine if it is torrent file (that we have)
  auto torrent_it =  std::find_if(m_torrentSegments.begin(), m_torrentSegments.end(), cmp);
  if (m_torrentSegments.end() != torrent_it) {
    data = std::make_shared<Data>(*torrent_it);
  }
  else {
    // determine if it is manifest (that we have)
    auto manifest_it = std::find_if(m_fileManifests.begin(), m_fileManifests.end(), cmp);
    if (m_fileManifests.end() != manifest_it) {
      data = std::make_shared<Data>(*manifest_it) ;
    }
    else {
      // determine if it is data packet (that we have)
      auto manifestName = interestName.getSubName(0, interestName.size() - 2);
      auto map_it = std::find_if(m_fileStates.begin(), m_fileStates.end(),
                                       [&manifestName](const std::pair<Name,
                                                                       std::vector<bool>>& kv){
                                        return manifestName.isPrefixOf(kv.first
                                          );
                                      });
      if (m_fileStates.end() != map_it) {
        auto packetName = interestName.getSubName(0, interestName.size() - 1);
        // get out the bitmap to be sure we have the packet
        auto& fileState = map_it->second;
        const auto &bitmap = fileState;
        auto packetNum = packetName.get(packetName.size() - 1).toSequenceNumber();
        if (bitmap[packetNum]) {
          // get the manifest
          auto manifest_it = std::find_if(m_fileManifests.begin(), m_fileManifests.end(),
                                          [&manifestName](const FileManifest& m) {
                                            return manifestName.isPrefixOf(m.name());
                                          });
          auto manifestFileName = manifest_it->file_name();
          auto filePath = m_dataPath + manifestFileName;
          data = IoUtil::readDataPacket(interestName,
                                        *manifest_it,
                                        m_subManifestSizes[manifestFileName],
                                        filePath);
        }
      }
    }
  }
  if (nullptr != data) {
    m_face->put(*data);
  }
  else {
    // TODO(msweatt) NACK
    LOG_ERROR << "NACK: " << interest << std::endl;
  }
  return;
}

void
TorrentManager::onRegisterFailed(const Name& prefix, const std::string& reason)
{
  LOG_ERROR << "ERROR: Failed to register prefix \""
            << prefix << "\" in local hub's daemon (" << reason << ")"
            << std::endl;
  shutdown();
}

shared_ptr<Interest>
TorrentManager::createInterest(Name name)
{
  shared_ptr<Interest> interest = make_shared<Interest>(name);
  interest->setInterestLifetime(time::milliseconds(2000));
  interest->setMustBeFresh(true);

  // Select routable prefix
  // Create and set the forwarding hint
  Delegation del;
  del.preference = 1;
  del.name = m_stats_table_iter->getRecordName();
  DelegationList list({del});

  // Stats Table update here...
  m_stats_table_iter->incrementSentInterests();

  m_sortingCounter++;
  if (m_sortingCounter >= SORTING_INTERVAL) {
    // Use the sorting interval to send out "ALIVE" Interests as well
    // check whether we should send out an "ALIVE" Interest
    if (m_updateHandler->needsUpdate()) {
      m_updateHandler->sendAliveInterest(m_stats_table_iter);
    }
    // Do the actual sorting related stuff
    m_sortingCounter = 0;
    m_statsTable.sort();
    m_stats_table_iter = m_statsTable.begin();
    m_retries = 0;
  }

  interest->setForwardingHint(list);

  return interest;
}

void
TorrentManager::nackCallBack(const Interest& i, const lp::Nack& n) {
  LOG_DEBUG << "Nack received: " << n.getReason() << ": " << i << std::endl;
  auto it = m_pendingInterests.find(i.getName());
  Name routablePrefix = (i.getForwardingHint().begin())->name;
  if (m_stats_table_iter->getRecordName() == routablePrefix) {
    m_stats_table_iter++;
    if (m_stats_table_iter == m_statsTable.end()) {
      m_stats_table_iter = m_statsTable.begin();
    }
  }
  Interest newInterest(i);
  m_stats_table_iter->incrementSentInterests();

  // Create and set the forwarding hint
  Delegation del;
  del.preference = 1;
  del.name = m_stats_table_iter->getRecordName();
  DelegationList list({del});

  if (m_updateHandler->needsUpdate()) {
    m_updateHandler->sendAliveInterest(m_stats_table_iter);
  }

  newInterest.setForwardingHint(list);
  LOG_DEBUG << "Resending Interest with LINK: " << m_stats_table_iter->getRecordName()
            << std::endl;

  m_face->expressInterest(newInterest, std::get<0>(it->second),
                          std::bind(&TorrentManager::nackCallBack, this, _1, _2),
                          std::get<1>(it->second));
 }

void
TorrentManager::sendInterest()
{
  while (m_pendingInterests.size() < WINDOW_SIZE && !m_interestQueue->empty()) {
    queueTuple tup = m_interestQueue->pop();
    m_pendingInterests.insert({std::get<0>(tup)->getName(), std::make_tuple(std::get<1>(tup),
                               std::get<2>(tup))});
    LOG_DEBUG << "Sending: " <<  *(std::get<0>(tup)) << std::endl;
    m_face->expressInterest(*std::get<0>(tup), std::get<1>(tup),
                            std::bind(&TorrentManager::nackCallBack, this, _1, _2),
                            std::get<2>(tup));
  }
}

void
TorrentManager::eraseOwnRoutablePrefix()
{
  Name ownRoutablePrefix = m_updateHandler->getOwnRoutablePrefix();
  if (m_statsTable.find(ownRoutablePrefix) != m_statsTable.end()) {
    LOG_DEBUG << "Erasing own routable prefix from StatsTable: " << ownRoutablePrefix
              << std::endl;
    std::cout << m_statsTable.erase(ownRoutablePrefix) << std::endl;
  }
  m_stats_table_iter = m_statsTable.begin();
  std::cout << m_statsTable.size() << std::endl;
  m_retries = 0;
}

}  // end ntorrent
}  // end ndn
