// Copyright (c) 2014, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Author: yanshiguang02@baidu.com

#include "chunkserver_impl.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>

#include <boost/bind.hpp>
#include <gflags/gflags.h>

#include <leveldb/db.h>
#include <leveldb/cache.h>

#include "common/mutex.h"
#include "common/atomic.h"
#include "common/counter.h"
#include "common/mutex.h"
#include "common/thread_pool.h"
#include "common/util.h"
#include "common/timer.h"
#include "common/sliding_window.h"
#include "common/string_util.h"
#include "file_cache.h"
#include "proto/nameserver.pb.h"
#include "rpc/rpc_client.h"

#include "chunkserver/counter_manager.h"
#include "chunkserver/data_block.h"

// Avoid conflict, we define LOG...
#include "common/logging.h"

DECLARE_string(block_store_path);
DECLARE_string(nameserver);
DECLARE_string(nameserver_port);
DECLARE_string(chunkserver_port);
DECLARE_int32(heartbeat_interval);
DECLARE_int32(blockreport_interval);
DECLARE_int32(blockreport_size);
DECLARE_int32(write_buf_size);
DECLARE_int32(chunkserver_work_thread_num);
DECLARE_int32(chunkserver_read_thread_num);
DECLARE_int32(chunkserver_write_thread_num);
DECLARE_int32(chunkserver_file_cache_size);
DECLARE_int32(chunkserver_max_pending_buffers);

namespace baidu {
namespace bfs {

extern common::Counter g_block_buffers;
extern common::Counter g_buffers_new;
extern common::Counter g_buffers_delete;
extern common::Counter g_blocks;
extern common::Counter g_writing_blocks;
extern common::Counter g_writing_bytes;
extern common::Counter g_find_ops;
extern common::Counter g_read_ops;
extern common::Counter g_write_ops;
extern common::Counter g_write_bytes;
extern common::Counter g_refuse_ops;
extern common::Counter g_rpc_delay;
extern common::Counter g_rpc_delay_all;
extern common::Counter g_rpc_count;
extern common::Counter g_data_size;

const int kUnknownChunkServerId = -1;

class BlockManager {
public:
    BlockManager(ThreadPool* thread_pool, const std::string& store_path)
        :_thread_pool(thread_pool),
         _metadb(NULL),
         _namespace_version(0), _disk_quota(0) {
         CheckStorePath(store_path);
         _file_cache = new FileCache(FLAGS_chunkserver_file_cache_size);
    }
    ~BlockManager() {
        for (BlockMap::iterator it = _block_map.begin();
                it != _block_map.end(); ++it) {
            it->second->DecRef();
        }
        _block_map.clear();
        delete _metadb;
        _metadb = NULL;
    }
    int64_t DiskQuota() {
        return _disk_quota;
    }
    void CheckStorePath(const std::string& store_path) {
        int64_t disk_quota = 0;
        common::SplitString(store_path, ",", &_store_path_list);
        for (uint32_t i = 0; i < _store_path_list.size(); ++i) {
           std::string& disk_path = _store_path_list[i];
           disk_path = common::TrimString(disk_path, " ");
           if (disk_path.empty() || disk_path[disk_path.size() - 1] != '/') {
               disk_path += "/";
           }
           struct statfs fs_info;
           if (0 == statfs(disk_path.c_str(), &fs_info)) {
               int64_t disk_size = fs_info.f_blocks * fs_info.f_bsize;
               int64_t user_quota = fs_info.f_bavail * fs_info.f_bsize;
               int64_t super_quota = fs_info.f_bfree * fs_info.f_bsize;
               LOG(INFO, "Use store path: %s block: %ld disk: %s available %s quota: %s",
                   disk_path.c_str(), fs_info.f_bsize,
                   common::HumanReadableString(disk_size).c_str(),
                   common::HumanReadableString(super_quota).c_str(),
                   common::HumanReadableString(user_quota).c_str());
               disk_quota += user_quota;
           } else {
               LOG(WARNING, "Stat store_path %s fail, ignore it", disk_path.c_str());
               _store_path_list[i] = _store_path_list[_store_path_list.size() - 1];
               _store_path_list.resize(_store_path_list.size() - 1);
               --i;
           }
        }
        std::sort(_store_path_list.begin(), _store_path_list.end());
        std::vector<std::string>::iterator it
           = std::unique(_store_path_list.begin(), _store_path_list.end());
        _store_path_list.resize(std::distance(_store_path_list.begin(), it));
        LOG(INFO, "%lu store path used.", _store_path_list.size());
        assert(_store_path_list.size() > 0);
        _disk_quota = disk_quota;
    }
    const std::string& GetStorePath(int64_t block_id) {
        return _store_path_list[block_id % _store_path_list.size()];
    }
    /// Load meta from disk
    bool LoadStorage() {
        MutexLock lock(&_mu);
        leveldb::Options options;
        options.create_if_missing = true;
        leveldb::Status s = leveldb::DB::Open(options, _store_path_list[0] + "meta/", &_metadb);
        if (!s.ok()) {
            LOG(WARNING, "Load blocks fail: %s", s.ToString().c_str());
            return false;
        }

        std::string version_key(8, '\0');
        version_key.append("version");
        std::string version_str;
        s = _metadb->Get(leveldb::ReadOptions(), version_key, &version_str);
        if (s.ok() && version_str.size() == 8) {
            _namespace_version = *(reinterpret_cast<int64_t*>(&version_str[0]));
            LOG(INFO, "Load namespace %ld", _namespace_version);
        }
        int block_num = 0;
        leveldb::Iterator* it = _metadb->NewIterator(leveldb::ReadOptions());
        for (it->Seek(version_key+'\0'); it->Valid(); it->Next()) {
            int64_t block_id = 0;
            if (1 != sscanf(it->key().data(), "%ld", &block_id)) {
                LOG(WARNING, "Unknown key: %s\n", it->key().ToString().c_str());
                delete it;
                return false;
            }
            BlockMeta meta;
            assert(it->value().size() == sizeof(meta));
            memcpy(&meta, it->value().data(), sizeof(meta));
            assert(meta.block_id == block_id);
            Block* block = new Block(meta, GetStorePath(block_id), _thread_pool, _file_cache);
            block->AddRef();
            _block_map[block_id] = block;
            block_num ++;
        }
        delete it;
        LOG(INFO, "Load %ld blocks, namespace version: %ld", block_num, _namespace_version);
        if (_namespace_version == 0 && block_num > 0) {
            LOG(WARNING, "Namespace version lost!");
        }
        _disk_quota += g_data_size.Get();
        return true;
    }
    int64_t NameSpaceVersion() const {
        return _namespace_version;
    }
    bool SetNameSpaceVersion(int64_t version) {
        MutexLock lock(&_mu);
        std::string version_key(8, '\0');
        version_key.append("version");
        std::string version_str(8, '\0');
        *(reinterpret_cast<int64_t*>(&version_str[0])) = version;
        leveldb::Status s = _metadb->Put(leveldb::WriteOptions(), version_key, version_str);
        if (!s.ok()) {
            LOG(WARNING, "SetNameSpaceVersion fail: %s", s.ToString().c_str());
            return false;
        }
        _namespace_version = version;
        LOG(INFO, "Set namespace version: %ld", _namespace_version);
        return true;
    }
    bool ListBlocks(std::vector<BlockMeta>* blocks, int64_t offset, int32_t num) {
        leveldb::Iterator* it = _metadb->NewIterator(leveldb::ReadOptions());
        for (it->Seek(BlockId2Str(offset)); it->Valid(); it->Next()) {
            int64_t block_id = 0;
            if (1 != sscanf(it->key().data(), "%ld", &block_id)) {
                LOG(WARNING, "[ListBlocks] Unknown meta key: %s\n",
                    it->key().ToString().c_str());
                delete it;
                return false;
            }
            BlockMeta meta;
            assert(it->value().size() == sizeof(meta));
            memcpy(&meta, it->value().data(), sizeof(meta));
            assert(meta.block_id == block_id);
            blocks->push_back(meta);
            // LOG(DEBUG, "List block %ld", block_id);
            if (--num <= 0) {
                break;
            }
        }
        delete it;
        return true;
    }
    Block* FindBlock(int64_t block_id, bool create_if_missing, int64_t* sync_time = NULL) {
        Block* block = NULL;
        {
            MutexLock lock(&_mu, "BlockManger::Find", 1000);
            g_find_ops.Inc();
            BlockMap::iterator it = _block_map.find(block_id);
            if (it != _block_map.end()) {
                block = it->second;
            } else if (create_if_missing) {
                BlockMeta meta;
                meta.block_id = block_id;
                meta.version = 0;
                block = new Block(meta, GetStorePath(block_id), _thread_pool, _file_cache);
                // for block_map
                block->AddRef();
                _block_map[block_id] = block;
                // Unlock for write meta & sync
                _mu.Unlock();
                if (!SyncBlockMeta(meta, sync_time)) {
                    delete block;
                    block = NULL;
                }
                _mu.Lock();
                if (!block) {
                    _block_map.erase(block_id);
                }
            } else {
                // not found
            }
        }
        // for user
        if (block) {
            block->AddRef();
        }
        return block;
    }
    std::string BlockId2Str(int64_t block_id) {
        char idstr[64];
        snprintf(idstr, sizeof(idstr), "%13ld", block_id);
        return std::string(idstr);
    }
    bool SyncBlockMeta(const BlockMeta& meta, int64_t* sync_time) {
        std::string idstr = BlockId2Str(meta.block_id);
        leveldb::WriteOptions options;
        // options.sync = true;
        int64_t time_start = common::timer::get_micros();
        leveldb::Status s = _metadb->Put(options, idstr,
            leveldb::Slice(reinterpret_cast<const char*>(&meta),sizeof(meta)));
        int64_t time_use = common::timer::get_micros() - time_start;
        if (sync_time) *sync_time = time_use;
        if (!s.ok()) {
            Log(WARNING, "Write to meta fail:%s", idstr.c_str());
            return false;
        }
        return true;
    }
    bool CloseBlock(Block* block) {
        if (!block->Close()) {
            return false;
        }

        // Update meta
        BlockMeta meta = block->GetMeta();
        return SyncBlockMeta(meta, NULL);
    }
    bool RemoveBlock(int64_t block_id) {
        Block* block = NULL;
        {
            MutexLock lock(&_mu, "BlockManager::RemoveBlock", 1000);
            BlockMap::iterator it = _block_map.find(block_id);
            if (it == _block_map.end()) {
                LOG(INFO, "Try to remove block that does not exist: #%ld ", block_id);
                return false;
            }
            block = it->second;
            if (!block->SetDeleted()) {
                LOG(INFO, "Block #%ld deleted by other thread", block_id);
                return false;
            }
            block->AddRef();
        }

        int64_t du = block->DiskUsed();
        std::string file_path = block->GetFilePath();
        _file_cache->EraseFileCache(file_path);
        int ret = remove(file_path.c_str());
        if (ret != 0 && (errno !=2 || du > 0)) {
            LOG(WARNING, "Remove #%ld disk file %s %ld bytes fails: %d (%s)",
                block_id, file_path.c_str(), du, errno, strerror(errno));
        } else {
            LOG(INFO, "Remove #%ld disk file done: %s\n",
                block_id, file_path.c_str());
        }

        char dir_name[5];
        snprintf(dir_name, sizeof(dir_name), "/%03ld", block_id % 1000);
        // Rmdir, ignore error when not empty.
        // rmdir((GetStorePath(block_id) + dir_name).c_str());
        char idstr[14];
        snprintf(idstr, sizeof(idstr), "%13ld", block_id);

        leveldb::Status s = _metadb->Delete(leveldb::WriteOptions(), idstr);
        if (s.ok()) {
            LOG(INFO, "Remove #%ld meta info done", block_id);
            {
                MutexLock lock(&_mu, "BlockManager::RemoveBlock erase", 1000);
                _block_map.erase(block_id);
            }
            block->DecRef();
            ret = true;
        } else {
            LOG(WARNING, "Remove #%ld meta info fails: %s", block_id, s.ToString().c_str());
            ret = false;
        }
        block->DecRef();
        return ret;
    }
private:
    ThreadPool* _thread_pool;
    std::vector<std::string> _store_path_list;
    typedef std::map<int64_t, Block*> BlockMap;
    BlockMap  _block_map;
    leveldb::DB* _metadb;
    FileCache* _file_cache;
    Mutex   _mu;
    int64_t _namespace_version;
    int64_t _disk_quota;
};

ChunkServerImpl::ChunkServerImpl()
    : _chunkserver_id(kUnknownChunkServerId) {
    _data_server_addr = common::util::GetLocalHostName() + ":" + FLAGS_chunkserver_port;
    _work_thread_pool = new ThreadPool(FLAGS_chunkserver_work_thread_num);
    _read_thread_pool = new ThreadPool(FLAGS_chunkserver_read_thread_num);
    _write_thread_pool = new ThreadPool(FLAGS_chunkserver_write_thread_num);
    _heartbeat_thread = new ThreadPool(1);
    _block_manager = new BlockManager(_write_thread_pool, FLAGS_block_store_path);
    bool s_ret = _block_manager->LoadStorage();
    assert(s_ret == true);
    _rpc_client = new RpcClient();
    std::string ns_address = FLAGS_nameserver + ":" + FLAGS_nameserver_port;
    if (!_rpc_client->GetStub(ns_address, &_nameserver)) {
        assert(0);
    }
    _counter_manager = new CounterManager;
    _work_thread_pool->AddTask(boost::bind(&ChunkServerImpl::LogStatus, this, true));
    _work_thread_pool->AddTask(boost::bind(&ChunkServerImpl::SendBlockReport, this));
    _heartbeat_thread->AddTask(boost::bind(&ChunkServerImpl::SendHeartbeat, this));
}

ChunkServerImpl::~ChunkServerImpl() {
    _work_thread_pool->Stop(true);
    _read_thread_pool->Stop(true);
    _write_thread_pool->Stop(true);
    _heartbeat_thread->Stop(true);
    delete _work_thread_pool;
    delete _read_thread_pool;
    delete _write_thread_pool;
    delete _heartbeat_thread;
    delete _block_manager;
    delete _rpc_client;
    delete _counter_manager;
    LogStatus(false);
}

void ChunkServerImpl::LogStatus(bool routine) {
    _counter_manager->GatherCounters();
    CounterManager::Counters counters = _counter_manager->GetCounters();

    LOG(INFO, "[Status] blocks %ld %ld buffers %ld data %sB, "
              "find %ld read %ld write %ld %ld %.2f MB, rpc_delay %ld %ld",
        g_writing_blocks.Get() ,g_blocks.Get(), g_block_buffers.Get(),
        common::HumanReadableString(g_data_size.Get()).c_str(),
        counters.find_ops, counters.read_ops,
        counters.write_ops, counters.refuse_ops,
        counters.write_bytes / 1024.0 / 1024,
        counters.rpc_delay, counters.delay_all);
    if (routine) {
        _work_thread_pool->DelayTask(1000,
            boost::bind(&ChunkServerImpl::LogStatus, this, true));
    }
}

void ChunkServerImpl::SendHeartbeat() {
    HeartBeatRequest request;
    request.set_chunkserver_id(_chunkserver_id);
    request.set_namespace_version(_block_manager->NameSpaceVersion());
    request.set_block_num(g_blocks.Get());
    request.set_data_size(g_data_size.Get());
    request.set_buffers(g_block_buffers.Get());
    HeartBeatResponse response;
    if (!_rpc_client->SendRequest(_nameserver, &NameServer_Stub::HeartBeat,
            &request, &response, 15, 1)) {
        LOG(WARNING, "Heat beat fail\n");
    } else if (_block_manager->NameSpaceVersion() != response.namespace_version()) {
        LOG(INFO, "Namespace version mismatch self:%ld ns:%ld",
            _block_manager->NameSpaceVersion(), response.namespace_version());
    }
    _heartbeat_thread->DelayTask(FLAGS_heartbeat_interval * 1000,
        boost::bind(&ChunkServerImpl::SendHeartbeat, this));
}

void ChunkServerImpl::SendBlockReport() {
    static int64_t last_report_blockid = -1;

    BlockReportRequest request;
    request.set_chunkserver_id(_chunkserver_id);
    request.set_chunkserver_addr(_data_server_addr);
    request.set_disk_quota(_block_manager->DiskQuota());
    request.set_namespace_version(_block_manager->NameSpaceVersion());

    std::vector<BlockMeta> blocks;
    _block_manager->ListBlocks(&blocks, last_report_blockid + 1, FLAGS_blockreport_size);

    int64_t blocks_num = blocks.size();
    for (int64_t i = 0; i < blocks_num; i++) {
        ReportBlockInfo* info = request.add_blocks();
        info->set_block_id(blocks[i].block_id);
        info->set_block_size(blocks[i].block_size);
        info->set_version(blocks[i].version);
    }

    if (blocks_num < FLAGS_blockreport_size) {
        last_report_blockid = -1;
        request.set_is_complete(true);
    } else {
        request.set_is_complete(false);
        if (blocks_num) {
            last_report_blockid = blocks[blocks_num - 1].block_id;
        }
    }

    BlockReportResponse response;
    if (!_rpc_client->SendRequest(_nameserver, &NameServer_Stub::BlockReport,
            &request, &response, 20, 3)) {
        LOG(WARNING, "Block reprot fail\n");
    } else {
        if (response.status() != 0) {
            LOG(FATAL, "Block report return %d", response.status());
        }
        int64_t new_version = response.namespace_version();
        if (_block_manager->NameSpaceVersion() != new_version) {
            // NameSpace change, chunkserver is empty.
            LOG(INFO, "New namespace version: %ld chunkserver id: %d",
                new_version, response.chunkserver_id());
            if (!_block_manager->SetNameSpaceVersion(new_version)) {
                LOG(FATAL, "Can not change namespace version");
            }
            _chunkserver_id = response.chunkserver_id();
        } else if (_chunkserver_id == kUnknownChunkServerId
                   && response.chunkserver_id() != kUnknownChunkServerId) {
            // Chunkserver restart
            _chunkserver_id = response.chunkserver_id();
            LOG(INFO, "Reconnect to nameserver version= %ld, new cs_id = %d",
                _block_manager->NameSpaceVersion(), _chunkserver_id);
        } else if (response.chunkserver_id() == kUnknownChunkServerId) {
            // Namespace change, chunkserver has old blocks.
            LOG(INFO, "Old chunkserver, namespace version: %ld, old_id: %d",
                _block_manager->NameSpaceVersion(), _chunkserver_id);
        } else if (_chunkserver_id != response.chunkserver_id()) {
            // Nameserver restart, chunkserver id change.
            LOG(INFO, "Chunkserver id change from %d to %d",
                _chunkserver_id, response.chunkserver_id());
            _chunkserver_id = response.chunkserver_id();
        }
        //LOG(INFO, "Report return old: %d new: %d", _chunkserver_id, response.chunkserver_id());
        //deal with obsolete blocks
        std::vector<int64_t> obsolete_blocks;
        for (int i = 0; i < response.obsolete_blocks_size(); i++) {
            obsolete_blocks.push_back(response.obsolete_blocks(i));
        }
        if (!obsolete_blocks.empty()) {
            boost::function<void ()> task =
                boost::bind(&ChunkServerImpl::RemoveObsoleteBlocks,
                            this, obsolete_blocks);
            _write_thread_pool->AddTask(task);
        }

        std::vector<ReplicaInfo> new_replica_info;
        for (int i = 0; i < response.new_replicas_size(); i++) {
            new_replica_info.push_back(response.new_replicas(i));
        }
        LOG(INFO, "Block report done. %lu replica blocks", new_replica_info.size());
        if (!new_replica_info.empty()) {
            boost::function<void ()> new_replica_task =
                boost::bind(&ChunkServerImpl::PullNewBlocks,
                            this, new_replica_info);
            _write_thread_pool->AddTask(new_replica_task);
        }
    }
    _work_thread_pool->DelayTask(FLAGS_blockreport_interval* 1000,
        boost::bind(&ChunkServerImpl::SendBlockReport, this));
}

bool ChunkServerImpl::ReportFinish(Block* block) {
    BlockReportRequest request;
    request.set_chunkserver_id(_chunkserver_id);
    request.set_chunkserver_addr(_data_server_addr);
    request.set_namespace_version(_block_manager->NameSpaceVersion());
    request.set_is_complete(false);

    ReportBlockInfo* info = request.add_blocks();
    info->set_block_id(block->Id());
    info->set_block_size(block->Size());
    info->set_version(0);
    BlockReportResponse response;
    if (!_rpc_client->SendRequest(_nameserver, &NameServer_Stub::BlockReport,
            &request, &response, 20, 3)) {
        LOG(WARNING, "Reprot finish fail: %ld\n", block->Id());
        return false;
    }

    LOG(INFO, "Report finish to nameserver done, block_id: %ld\n", block->Id());
    return true;
}

void ChunkServerImpl::WriteBlock(::google::protobuf::RpcController* controller,
                        const WriteBlockRequest* request,
                        WriteBlockResponse* response,
                        ::google::protobuf::Closure* done) {
    int64_t block_id = request->block_id();
    const std::string& databuf = request->databuf();
    int64_t offset = request->offset();
    int32_t packet_seq = request->packet_seq();

    if (!response->has_sequence_id()) {
        response->set_sequence_id(request->sequence_id());
        /// Flow control
        if (g_block_buffers.Get() > FLAGS_chunkserver_max_pending_buffers
            || _work_thread_pool->PendingNum() > FLAGS_chunkserver_max_pending_buffers) {
            response->set_status(500);
            LOG(WARNING, "[WriteBlock] pending buf[%ld] req[%ld] reject #%ld seq:%d, offset:%ld, len:%lu ts:%lu\n",
                g_block_buffers.Get(), _work_thread_pool->PendingNum(),
                block_id, packet_seq, offset, databuf.size(), request->sequence_id());
            done->Run();
            g_refuse_ops.Inc();
            return;
        }
        LOG(DEBUG, "[WriteBlock] dispatch #%ld seq:%d, offset:%ld, len:%lu] %lu\n",
           block_id, packet_seq, offset, databuf.size(), request->sequence_id());
        response->add_timestamp(common::timer::get_micros());
        boost::function<void ()> task =
            boost::bind(&ChunkServerImpl::WriteBlock, this, controller, request, response, done);
        _work_thread_pool->AddTask(task);
        return;
    }

    response->add_timestamp(common::timer::get_micros());
    LOG(INFO, "[WriteBlock] #%ld seq:%d, offset:%ld, len:%lu",
           block_id, packet_seq, offset, databuf.size());

    if (request->chunkservers_size()) {
        // New request for next chunkserver
        WriteBlockRequest* next_request = new WriteBlockRequest(*request);
        WriteBlockResponse* next_response = new WriteBlockResponse();
        next_request->clear_chunkservers();
        for (int i = 1; i < request->chunkservers_size(); i++) {
            next_request->add_chunkservers(request->chunkservers(i));
        }
        ChunkServer_Stub* stub = NULL;
        const std::string& next_server = request->chunkservers(0);
        _rpc_client->GetStub(next_server, &stub);
        WriteNext(next_server, stub, next_request, next_response, request, response, done);
    } else {
        boost::function<void ()> callback =
            boost::bind(&ChunkServerImpl::LocalWriteBlock, this, request, response, done);
        _work_thread_pool->AddTask(callback);
    }
}

void ChunkServerImpl::WriteNext(const std::string& next_server,
                                ChunkServer_Stub* stub,
                                const WriteBlockRequest* next_request,
                                WriteBlockResponse* next_response,
                                const WriteBlockRequest* request,
                                WriteBlockResponse* response,
                                ::google::protobuf::Closure* done) {
    int64_t block_id = request->block_id();
    int32_t packet_seq = request->packet_seq();
    LOG(INFO, "[WriteBlock] send #%ld seq:%d to next %s\n",
        block_id, packet_seq, next_server.c_str());
    boost::function<void (const WriteBlockRequest*, WriteBlockResponse*, bool, int)> callback =
        boost::bind(&ChunkServerImpl::WriteNextCallback,
            this, _1, _2, _3, _4, next_server, std::make_pair(request, response), done, stub);
    _rpc_client->AsyncRequest(stub, &ChunkServer_Stub::WriteBlock,
        next_request, next_response, callback, 30, 3);
}

void ChunkServerImpl::WriteNextCallback(const WriteBlockRequest* next_request,
                        WriteBlockResponse* next_response,
                        bool failed, int error,
                        const std::string& next_server,
                        std::pair<const WriteBlockRequest*, WriteBlockResponse*> origin,
                        ::google::protobuf::Closure* done,
                        ChunkServer_Stub* stub) {
    const WriteBlockRequest* request = origin.first;
    WriteBlockResponse* response = origin.second;
    /// If RPC_ERROR_SEND_BUFFER_FULL retry send.
    if (failed && error == sofa::pbrpc::RPC_ERROR_SEND_BUFFER_FULL) {
        boost::function<void ()> callback =
            boost::bind(&ChunkServerImpl::WriteNext, this, next_server,
                        stub, next_request, next_response, request, response, done);
        _work_thread_pool->DelayTask(10, callback);
        return;
    }
    delete next_request;
    delete stub;

    int64_t block_id = request->block_id();
    const std::string& databuf = request->databuf();
    int64_t offset = request->offset();
    int32_t packet_seq = request->packet_seq();
    if (failed || next_response->status() != 0) {
        LOG(WARNING, "[WriteBlock] WriteNext %s fail: #%ld seq:%d, offset:%ld, len:%lu], "
                     "status= %d, error= %d\n",
            next_server.c_str(), block_id, packet_seq, offset, databuf.size(),
            next_response->status(), error);
        if (next_response->status() == 0) {
            response->set_status(error);
        } else {
            response->set_status(next_response->status());
        }
        delete next_response;
        done->Run();
        return;
    } else {
        LOG(INFO, "[Writeblock] send #%ld seq:%d to next done", block_id, packet_seq);
        delete next_response;
    }

    boost::function<void ()> callback =
        boost::bind(&ChunkServerImpl::LocalWriteBlock, this, request, response, done);
    _work_thread_pool->AddTask(callback);
}

void ChunkServerImpl::LocalWriteBlock(const WriteBlockRequest* request,
                        WriteBlockResponse* response,
                        ::google::protobuf::Closure* done) {
    int64_t block_id = request->block_id();
    const std::string& databuf = request->databuf();
    int64_t offset = request->offset();
    int32_t packet_seq = request->packet_seq();

    if (!response->has_status()) {
        response->set_status(0);
    }

    int64_t find_start = common::timer::get_micros();
    /// search;
    int64_t sync_time = 0;
    Block* block = _block_manager->FindBlock(block_id, true, &sync_time);
    if (!block) {
        LOG(WARNING, "[WriteBlock] Block not found: #%ld ", block_id);
        response->set_status(8404);
        done->Run();
        return;
    }

    int64_t add_used = 0;
    int64_t write_start = common::timer::get_micros();
    if (!block->Write(packet_seq, offset, databuf.data(), databuf.size(), &add_used)) {
        block->DecRef();
        response->set_status(812);
        done->Run();
        return;
    }
    int64_t write_end = common::timer::get_micros();
    if (request->is_last()) {
        block->SetSliceNum(packet_seq + 1);
        block->SetVersion(packet_seq);
    }

    // If complete, close block, and report only once(close block return true).
    int64_t report_start = write_end;
    if (block->IsComplete() && _block_manager->CloseBlock(block)) {
        LOG(INFO, "[WriteBlock] block finish #%ld size:%ld", block_id, block->Size());
        report_start = common::timer::get_micros();
        ReportFinish(block);
    }

    int64_t time_end = common::timer::get_micros();
    LOG(INFO, "[WriteBlock] done #%ld seq:%d, offset:%ld, len:%lu "
              "use %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld ms",
        block_id, packet_seq, offset, databuf.size(),
        (response->timestamp(0) - request->sequence_id()) / 1000, // recv
        (response->timestamp(1) - response->timestamp(0)) / 1000, // dispatch time
        (find_start - response->timestamp(1)) / 1000, // async time
        (write_start - find_start - sync_time) / 1000,  // find time
        sync_time / 1000, // create sync time
        add_used / 1000, // sliding window add
        (write_end - write_start) / 1000,    // write time
        (report_start - write_end) / 1000, // close time
        (time_end - report_start) / 1000, // report time
        (time_end - response->timestamp(0)) / 1000); // total time
    g_rpc_delay.Add(response->timestamp(0) - request->sequence_id());
    g_rpc_delay_all.Add(time_end - request->sequence_id());
    g_rpc_count.Inc();
    g_write_ops.Inc();
    done->Run();
    block->DecRef();
    block = NULL;
}

void ChunkServerImpl::ReadBlock(::google::protobuf::RpcController* controller,
                        const ReadBlockRequest* request,
                        ReadBlockResponse* response,
                        ::google::protobuf::Closure* done) {
    if (!response->has_sequence_id()) {
        response->set_sequence_id(request->sequence_id());
        response->add_timestamp(common::timer::get_micros());
        boost::function<void ()> task =
            boost::bind(&ChunkServerImpl::ReadBlock, this, controller, request, response, done);
        _read_thread_pool->AddTask(task);
        return;
    }

    int64_t block_id = request->block_id();
    int64_t offset = request->offset();
    int32_t read_len = request->read_len();
    int status = 0;

    int64_t find_start = common::timer::get_micros();
    Block* block = _block_manager->FindBlock(block_id, false);
    if (block == NULL) {
        status = 404;
        LOG(WARNING, "ReadBlock not found: #%ld offset: %ld len: %d\n",
                block_id, offset, read_len);
    } else {
        int64_t read_start = common::timer::get_micros();
        char* buf = new char[read_len];
        int32_t len = block->Read(buf, read_len, offset);
        int64_t read_end = common::timer::get_micros();
        if (len >= 0) {
            response->mutable_databuf()->assign(buf, len);
            if (request->require_block_version()) {
                response->set_block_version(block->GetVersion());
            }
            LOG(INFO, "ReadBlock #%ld offset: %ld len: %d return: %d "
                      "use %ld %ld %ld %ld %ld",
                block_id, offset, read_len, len,
                (response->timestamp(0) - request->sequence_id()) / 1000, // rpc time
                (find_start - response->timestamp(0)) / 1000,   // dispatch time
                (read_start - find_start) / 1000, // find time
                (read_end - read_start) / 1000,  // read time
                (read_end - response->timestamp(0)) / 1000);    // service time
            g_read_ops.Inc();
        } else {
            status = 882;
            LOG(WARNING, "ReadBlock #%ld fail offset: %ld len: %d\n",
                block_id, offset, read_len);
        }
        delete[] buf;
    }
    response->set_status(status);
    done->Run();
    if (block) {
        block->DecRef();
    }
}
void ChunkServerImpl::RemoveObsoleteBlocks(std::vector<int64_t> blocks) {
    for (size_t i = 0; i < blocks.size(); i++) {
        if (!_block_manager->RemoveBlock(blocks[i])) {
            LOG(INFO, "Remove block fail: #%ld ", blocks[i]);
        }
    }
}
void ChunkServerImpl::PullNewBlocks(std::vector<ReplicaInfo> new_replica_info) {
    PullBlockReportRequest report_request;
    report_request.set_sequence_id(0);
    report_request.set_chunkserver_id(_chunkserver_id);
    for (size_t i = 0; i < new_replica_info.size(); i++) {
        int64_t block_id = new_replica_info[i].block_id();
        ChunkServer_Stub* chunkserver = NULL;
        Block* block = _block_manager->FindBlock(block_id, true);
        if (!block) {
            LOG(WARNING, "Cant't create block: #%ld ", block_id);
            //ignore this block
            continue;
        } else {
            LOG(INFO, "Start pull #%ld from %s",
                block_id, new_replica_info[i].chunkserver_address(0).c_str());
        }
        int init_index = 0;
        for (; init_index < new_replica_info[i].chunkserver_address_size(); init_index++) {
            if (_rpc_client->GetStub(new_replica_info[i].chunkserver_address(init_index), &chunkserver)) {
                break;
            }
        }
        if (init_index == new_replica_info[i].chunkserver_address_size()) {
             LOG(WARNING, "Can't connect to any chunkservers for pull block #%ld\n", block_id);
            //remove this block
            block->DecRef();
            _block_manager->RemoveBlock(block_id);
            report_request.add_blocks(block_id);
            continue;
        }
        int64_t seq = -1;
        int64_t offset = 0;
        bool success = true;
        int pre_index = init_index;
        while (1) {
            ReadBlockRequest request;
            ReadBlockResponse response;
            request.set_sequence_id(++seq);
            request.set_block_id(block_id);
            request.set_offset(offset);
            request.set_read_len(256 * 1024);
            request.set_require_block_version(true);
            bool ret = _rpc_client->SendRequest(chunkserver,
                                                &ChunkServer_Stub::ReadBlock,
                                                &request, &response, 15, 3);
            if (!ret || response.status() != 0) {
                //try another chunkserver
                //reset seq
                --seq;
                delete chunkserver;
                pre_index = (pre_index + 1) % new_replica_info[i].chunkserver_address_size();
                LOG(INFO, "Change src chunkserver to %s for pull block #%ld",
                        new_replica_info[i].chunkserver_address(pre_index).c_str(), block_id);
                if (pre_index == init_index) {
                    success = false;
                    break;
                } else {
                    _rpc_client->GetStub(new_replica_info[i].chunkserver_address(pre_index), &chunkserver);
                    continue;
                }
            }
            int32_t len = response.databuf().size();
            const char* buf = response.databuf().data();
            if (len) {
                if (!block->Write(seq, offset, buf, len)) {
                    success = false;
                    break;
                }
            } else {
                block->SetSliceNum(seq);
                block->SetVersion(response.block_version());
            }
            if (block->IsComplete() && _block_manager->CloseBlock(block)) {
                LOG(INFO, "Pull block: #%ld finish\n", block_id);
                break;
            }
            offset += len;
        }
        delete chunkserver;
        block->DecRef();
        if (!success) {
            _block_manager->RemoveBlock(block_id);
        }
        report_request.add_blocks(block_id);
    }

    PullBlockReportResponse report_response;
    if (!_rpc_client->SendRequest(_nameserver, &NameServer_Stub::PullBlockReport,
                &report_request, &report_response, 15, 3)) {
        LOG(WARNING, "Report pull finish fail, chunkserver id: %d\n", _chunkserver_id);
    } else {
        LOG(INFO, "Report pull finish dnne, %d blocks", report_request.blocks_size());
    }
}


void ChunkServerImpl::GetBlockInfo(::google::protobuf::RpcController* controller,
                                   const GetBlockInfoRequest* request,
                                   GetBlockInfoResponse* response,
                                   ::google::protobuf::Closure* done) {
    if (!response->has_sequence_id()) {
        response->set_sequence_id(request->sequence_id());
        response->add_timestamp(common::timer::get_micros());
        boost::function<void ()> task =
            boost::bind(&ChunkServerImpl::GetBlockInfo, this, controller, request, response, done);
        _read_thread_pool->AddTask(task);
        return;
    }
    int64_t block_id = request->block_id();
    int status = 0;

    int64_t find_start = common::timer::get_micros();
    Block* block = _block_manager->FindBlock(block_id, false);
    int64_t find_end = common::timer::get_micros();
    if (block == NULL) {
        status = 404;
        LOG(WARNING, "GetBlockInfo not found: #%ld ",block_id);
    } else {
        int64_t block_size = block->GetMeta().block_size;
        response->set_block_size(block_size);
        status = 0;
        LOG(INFO, "GetBlockInfo #%ld return: %d "
                  "use %ld %ld %ld %ld",
            block_id, block_size,
            (response->timestamp(0) - request->sequence_id()) / 1000, // rpc time
            (find_start - response->timestamp(0)) / 1000,   // dispatch time
            (find_end - find_start) / 1000, // find time
            (find_end - response->timestamp(0)) / 1000);    // service time
    }
    response->set_status(status);
    done->Run();

    if (block) block->DecRef();

}

bool ChunkServerImpl::WebService(const sofa::pbrpc::HTTPRequest& request,
                                sofa::pbrpc::HTTPResponse& response) {
    CounterManager::Counters counters = _counter_manager->GetCounters();
    std::string str =
            "<html><head><title>BFS console</title>"
            "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />"
            "<link rel=\"stylesheet\" type=\"text/css\" "
                "href=\"http://www.w3school.com.cn/c5.css\"/>"
            "<style> body { background: #f9f9f9;}"
            "</style>"
            "</head>";
    str += "<body> <h1>分布式文件系统控制台 - ChunkServer</h1>";
    str += "<table class=dataintable>";
    str += "<tr><td>Block number</td><td>Data size</td>"
           "<td>Write(QPS)</td><td>Write(Speed)<td>Read(QPS)</td><td>Buffers(new/delete)</td><tr>";
    str += "<tr><td>" + common::NumToString(g_blocks.Get()) + "</td>";
    str += "<td>" + common::HumanReadableString(g_data_size.Get()) + "</td>";
    str += "<td>" + common::NumToString(counters.write_ops) + "</td>";
    str += "<td>" + common::HumanReadableString(counters.write_bytes) + "/S</td>";
    str += "<td>" + common::NumToString(counters.read_ops) + "</td>";
    str += "<td>" + common::NumToString(g_block_buffers.Get())
           + "(" + common::NumToString(counters.buffers_new) + "/"
           + common::NumToString(counters.buffers_delete) +")" + "</td>";
    str += "</tr>";
    str += "</table>";
    str += "<script> var int = setInterval('window.location.reload()', 1000);"
           "function check(box) {"
           "if(box.checked) {"
           "    int = setInterval('window.location.reload()', 1000);"
           "} else {"
           "    clearInterval(int);"
           "}"
           "}</script>"
           "<input onclick=\"javascript:check(this)\" "
           "checked=\"checked\" type=\"checkbox\">自动刷新</input>";
    str += "</body></html>";
    response.content = str;
    return true;
}
} // namespace bfs
} //namespace baidu

/* vim: set expandtab ts=4 sw=4 sts=4 tw=100: */
