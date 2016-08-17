//
// Created by terark on 16-8-2.
//

#ifndef TERARKDB_TEST_FRAMEWORK_BENCHMARK_H
#define TERARKDB_TEST_FRAMEWORK_BENCHMARK_H
#include <sys/types.h>
#include <sys/time.h>
#include <sstream>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include "db/db_impl.h"
#include "db/version_set.h"
#include "include/leveldb/cache.h"
#include "include/leveldb/db.h"
#include "include/leveldb/env.h"
#include "include/leveldb/write_batch.h"
#include "port/port.h"
#include "util/crc32c.h"
#include "util/histogram.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "util/testutil.h"

#include <iostream>
#include <fstream>
#include <string.h>
#include <string>

#include <terark/db/db_table.hpp>
#include <terark/io/MemStream.hpp>
#include <terark/io/DataIO.hpp>
#include <terark/io/RangeStream.hpp>
#include <terark/lcast.hpp>
#include <terark/util/autofree.hpp>
#include <terark/util/fstrvec.hpp>
#include <port/port_posix.h>
#include <src/Setting.h>
#include <thread>
#include "src/leveldb.h"
#include <tbb/concurrent_vector.h>
#include <stdint.h>
#include <stdint-gcc.h>
//terarkdb -update_data_path=/mnt/hdd/data/xab --benchmarks=fillrandom --num=45 --sync_index=1 --db=./experiment/new_wiki --resource_data=/dev/stdin --threads=1 --keys_data=/home/terark/Documents/data/wiki_keys

using namespace terark;
using namespace db;
struct ThreadState {
    int tid;             // 0..n-1 when running in n threads
    Stats stats;
    std::atomic<uint8_t> STOP;
    WT_SESSION *session;
    std::atomic<std::vector<uint8_t >*> *whichExecutePlan;
    std::atomic<std::vector<uint8_t >*> *whichSamplingPlan;

    ThreadState(int index,Setting &setting1,std::atomic<std::vector<uint8_t >*>* wep,
                std::atomic<std::vector<uint8_t >*>* wsp)
            :   tid(index),
                whichExecutePlan(wep),
                whichSamplingPlan(wsp)
    {
        STOP.store(false);
    }
    ThreadState(int index,Setting &setting1,WT_CONNECTION *conn,
                std::atomic<std::vector<uint8_t >*>* wep,std::atomic<std::vector<uint8_t >*>* wsp)
            :tid(index),
             whichExecutePlan(wep),
             whichSamplingPlan(wsp){
        conn->open_session(conn, NULL, NULL, &session);
        STOP.store(false);
        assert(session != NULL);
    }
    ~ThreadState(){
    }
};
class Benchmark{
private:
    std::unordered_map<uint8_t, bool (Benchmark::*)(ThreadState *)> executeFuncMap;
    std::unordered_map<uint8_t, bool (Benchmark::*)(ThreadState *,uint8_t)> samplingFuncMap;
    std::unordered_map<uint8_t ,uint8_t > samplingRecord;
    std::atomic<std::vector<uint8_t > *> executePlanAddr;
    std::atomic<std::vector<uint8_t > *> samplingPlanAddr;
    std::vector<uint8_t > executePlan[2];
    std::vector<uint8_t > samplingPlan[2];
    bool whichEPlan = false;//不作真假，只用来切换plan
    bool whichSPlan = false;
    std::ifstream keysFile;
    std::ifstream insertFile;
    size_t updateKeys(void);
    void backupKeys(void);
public:
    std::ifstream loadFile;
    std::vector<std::pair<std::thread,ThreadState*>> threads;
    Setting &setting;
    static tbb::concurrent_queue<std::string> updateDataCq;
    static tbb::concurrent_vector<std::string> allkeys;
    static void loadInsertData(std::ifstream *ifs,Setting *setting){
        assert(ifs->is_open());
        std::string str;
        std::cout << "loadInsertData start" << std::endl;
        while( setting->ifStop() == false && !ifs->eof()){

            while( updateDataCq.unsafe_size() < 20000){
                if (ifs->eof())
                    break;
                getline(*ifs,str);
                updateDataCq.push(str);
            }
            sleep(3);
        }
        std::cout << "loadInsertData stop" << std::endl;
    }
    Benchmark(Setting &s):setting(s){
        executeFuncMap[1] = &Benchmark::ReadOneKey;
        executeFuncMap[0] = &Benchmark::UpdateOneKey;
        executeFuncMap[2] = &Benchmark::InsertOneKey;
        samplingFuncMap[0] = &Benchmark::executeOneOperationWithoutSampling;
        samplingFuncMap[1] = &Benchmark::executeOneOperationWithSampling;

        insertFile.open(setting.getInsertDataPath());
        assert(insertFile.is_open());
        loadFile.open(setting.getLoadDataPath());
        assert(loadFile.is_open());
        keysFile.open(setting.getKeysDataPath());
        assert(keysFile.is_open());
    };
    virtual void  Run(void) final {
        Open();

        std::cout << setting.ifRunOrLoad() << std::endl;
        if (setting.ifRunOrLoad() == "load") {
            allkeys.clear();
            Load();
            backupKeys();
        }
        else {
            std::cout << "allKeys size:" <<updateKeys() << std::endl;
            RunBenchmark();
        }
        Close();
    };

    virtual void Open(void) = 0;
    virtual void Load(void) = 0;
    virtual void Close(void) = 0;
    virtual bool ReadOneKey(ThreadState*) = 0;
    virtual bool UpdateOneKey(ThreadState *) = 0;
    virtual bool InsertOneKey(ThreadState *) = 0;
    virtual ThreadState* newThreadState(std::atomic<std::vector<uint8_t >*>* whichExecutePlan,
                                        std::atomic<std::vector<uint8_t >*>* whichSamplingPlan) = 0;

    static void ThreadBody(Benchmark *bm,ThreadState* state){
        bm->ReadWhileWriting(state);
    }
    void updatePlan(std::vector<uint8_t> &plan, std::vector<std::pair<uint8_t ,uint8_t >> percent,uint8_t defaultVal);

    void shufflePlan(std::vector<uint8_t > &plan);

    void adjustThreadNum(uint32_t target, std::atomic<std::vector<uint8_t >*>* whichEPlan,
                         std::atomic<std::vector<uint8_t >*>* whichSPlan);
    void adjustExecutePlan(uint8_t readPercent,uint8_t insertPercent);
    void adjustSamplingPlan(uint8_t samplingRate);
    void RunBenchmark(void);
    bool executeOneOperationWithSampling(ThreadState* state,uint8_t type);
    bool executeOneOperationWithoutSampling(ThreadState* state,uint8_t type);
    bool executeOneOperation(ThreadState* state,uint8_t type);
    void ReadWhileWriting(ThreadState *thread);

    std::string GatherTimeData();
};
class TerarkBenchmark : public Benchmark{
private:
    DbTablePtr tab;
    DbContextPtr ctx;
public:

    TerarkBenchmark(Setting &setting1) : tab(NULL), Benchmark(setting1){};

    ~TerarkBenchmark() {
        tab->safeStopAndWaitForCompress();
        tab = NULL;
    }
private:
    ThreadState* newThreadState(std::atomic<std::vector<uint8_t >*>* whichEPlan,
                                std::atomic<std::vector<uint8_t >*>* whichSPlan){
        return new ThreadState(threads.size(),setting,whichEPlan,whichSPlan);
    }
    void PrintHeader() {
        fprintf(stdout, "NarkDB Test Begins!");
    }
    void Close(){tab->safeStopAndWaitForFlush();tab = NULL;};
    void Load(void){
        DoWrite(true);
        tab->compact();
    }
    std::string getKey(std::string &str){
        std::vector<std::string> strvec;
        boost::split(strvec,str,boost::is_any_of("\t"));
        assert(strvec.size() > 7);
//        valvec<byte_t> encodedKey;
//        tab->getIndexSchema(0);
//        tab->getIndexSchema("title,timestamp");
//        size_t n = indexSchema.parseDelimText(key, &encodedKey);
        return strvec[2] + '\0' + strvec[7];
    }
    void Open() {
        PrintHeader();
        assert(tab == NULL);
        std::cout << "Open database " << setting.FLAGS_db<< std::endl;
        tab = CompositeTable::open(setting.FLAGS_db);
        assert(tab != NULL);
        ctx = tab->createDbContext();
        ctx->syncIndex = setting.FLAGS_sync_index;
        std::cout << "Open success!" <<std::endl;
    }
    void DoWrite( bool seq) {
        std::cout << "Write the data to terark : " << setting.getLoadDataPath() << std::endl;
        assert(loadFile.is_open());
        std::string str;
        long long recordnumber = 0;
        const Schema& rowSchema = tab->rowSchema();
        valvec<byte_t> row;
        while(getline(loadFile, str)) {
            rowSchema.parseDelimText('\t', str, &row);
            if (ctx->upsertRow(row) < 0) { // unique index
                printf("Insert failed: %s\n", ctx->errMsg.c_str());
            }
            allkeys.push_back(getKey(str));
            recordnumber++;
            if ( recordnumber % 1000 == 0)
                std::cout << "Insert reocord number: " << recordnumber << std::endl;
        }
        time_t now;
        struct tm *timenow;
        time(&now);
        timenow = localtime(&now);
        printf("recordnumber %lld, time %s\n",recordnumber, asctime(timenow));
    }
    bool ReadOneKey(ThreadState *thread) {
        if (allkeys.size() == 0)
            return false;
        valvec<byte> keyHit, val;
        valvec<valvec<byte> > cgDataVec;
        valvec<llong> idvec;
        valvec<size_t> colgroups;
        int found = 0;
        for (size_t i = tab->getIndexNum(); i < tab->getColgroupNum(); i++) {
            colgroups.push_back(i);
        }
        size_t indexId = tab->getIndexId("cur_title,cur_timestamp");
        fstring key(allkeys.at(rand() % allkeys.size()));
        tab->indexSearchExact(indexId, key, &idvec, ctx.get());
        assert(idvec.size() <= 1);
        if (idvec.size() == 0)
            return false;
        valvec<byte> rowVal;
        ctx->getValue(idvec[0], &rowVal);
//        tab->selectColgroups(idvec[0], colgroups, &cgDataVec, ctx.get());
//        std::cout << idvec[0] << std::endl;
//        fstring test(cgDataVec[0]);
//        std::cout << test << std::endl;
        return true;
    }
    bool ReadOneKey(ThreadState *thread,std::string &key,valvec<byte>& rowVal,llong &rid) {
        if (allkeys.size() == 0)
            return false;
        valvec<byte> keyHit, val;
        valvec<llong> idvec;
        valvec<size_t> colgroups;

        for (size_t i = tab->getIndexNum(); i < tab->getColgroupNum(); i++) {
            colgroups.push_back(i);
        }

        size_t indexId = tab->getIndexId("cur_title,cur_timestamp");
        tab->indexSearchExact(indexId, key, &idvec, ctx.get());
        assert(idvec.size() <= 1);
        if (idvec.size() == 0)
            return false;
        ctx->getValue(idvec[0], &rowVal);
        rid = idvec[0];
        return true;
    }
    bool UpdateOneKey(ThreadState *thread) {
        valvec<byte> rowVal;
        llong rid;
        if (allkeys.size() == 0)
            return false;
        if ( ReadOneKey(thread,allkeys.at(rand() % allkeys.size()),rowVal,rid) == false){
            return false;
        }
        ctx->updateRow(rid,rowVal);
        return true;
    }

    bool InsertOneKey(ThreadState *thread){
        std::string str;

        if (updateDataCq.try_pop(str) == false) {
            return false;
        }
        //std::cout << str << std::endl;
        const Schema &rowSchema = tab->rowSchema();
        valvec<byte_t> row;
        if (rowSchema.columnNum() != rowSchema.parseDelimText('\t', str, &row))
            return false;
        if (ctx->upsertRow(row) < 0) { // unique index
            printf("Insert failed: %s\n", ctx->errMsg.c_str());
        }
        //allkeys.push_back(getKey(str));
        //std::cout << allkeys.back() << std::endl;
        return true;
    }
};
using namespace leveldb;
class WiredTigerBenchmark : public Benchmark{
private:
    WT_CONNECTION *conn_;
    std::string uri_;
    int db_num_;
    int num_;
    int sync_;
    void PrintHeader() {
        const int kKeySize = 16;
        PrintEnvironment();
        fprintf(stdout, "Keys:       %d bytes each\n", kKeySize);
        fprintf(stdout, "Values:     %d bytes each (%d bytes after compression)\n",
                setting.FLAGS_value_size,
                static_cast<int>(setting.FLAGS_value_size * setting.FLAGS_compression_ratio + 0.5));
        fprintf(stdout, "Entries:    %d\n", num_);
        fprintf(stdout, "RawSize:    %.1f MB (estimated)\n",
                ((static_cast<int64_t>(kKeySize + setting.FLAGS_value_size) * num_)
                 / 1048576.0));
        fprintf(stdout, "FileSize:   %.1f MB (estimated)\n",
                (((kKeySize + setting.FLAGS_value_size * setting.FLAGS_compression_ratio) * num_)
                 / 1048576.0));
        PrintWarnings();
        fprintf(stdout, "------------------------------------------------\n");
    }
    void PrintWarnings() {
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
        fprintf(stdout,
                "WARNING: Optimization is disabled: benchmarks unnecessarily slow\n"
        );
#endif
#ifndef NDEBUG
        fprintf(stdout,
                "WARNING: Assertions are enabled; benchmarks unnecessarily slow\n");
#endif

        // See if snappy is working by attempting to compress a compressible string
        const char text[] = "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy";
        std::string compressed;
        if (!port::Snappy_Compress(text, sizeof(text), &compressed)) {
            fprintf(stdout, "WARNING: Snappy compression is not enabled\n");
        } else if (compressed.size() >= sizeof(text)) {
            fprintf(stdout, "WARNING: Snappy compression is not effective\n");
        }
    }
    void PrintEnvironment() {
        int wtmaj, wtmin, wtpatch;
        const char *wtver = wiredtiger_version(&wtmaj, &wtmin, &wtpatch);
        fprintf(stdout, "WiredTiger:    version %s, lib ver %d, lib rev %d patch %d\n",
                wtver, wtmaj, wtmin, wtpatch);
        fprintf(stderr, "WiredTiger:    version %s, lib ver %d, lib rev %d patch %d\n",
                wtver, wtmaj, wtmin, wtpatch);

#if defined(__linux)
        time_t now = time(NULL);
        fprintf(stderr, "Date:       %s", ctime(&now));  // ctime() adds newline

        FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
        if (cpuinfo != NULL) {
            char line[1000];
            int num_cpus = 0;
            std::string cpu_type;
            std::string cache_size;
            while (fgets(line, sizeof(line), cpuinfo) != NULL) {
                const char* sep = strchr(line, ':');
                if (sep == NULL) {
                    continue;
                }
                Slice key = TrimSpace(Slice(line, sep - 1 - line));
                Slice val = TrimSpace(Slice(sep + 1));
                if (key == "model name") {
                    ++num_cpus;
                    cpu_type = val.ToString();
                } else if (key == "cache size") {
                    cache_size = val.ToString();
                }
            }
            fclose(cpuinfo);
            fprintf(stderr, "CPU:        %d * %s\n", num_cpus, cpu_type.c_str());
            fprintf(stderr, "CPUCache:   %s\n", cache_size.c_str());
        }
#endif
    }
public:
    WiredTigerBenchmark(Setting &setting1) : Benchmark(setting1) {

    }
    ~WiredTigerBenchmark() {
    }
private:
    size_t getKeyAndValue(std::string &str,std::string &key,std::string &val){
        std::vector<std::string> strvec;
        boost::split(strvec,str,boost::is_any_of("\t"));
        assert(key.size() + val.size() == 0);
        key = strvec[2] + strvec[7];
        for(int i = 0; i < strvec.size(); i ++){
            if (i == 2 || i == 7)
                continue;
            val += strvec[i];
        }
        return strvec.size();
    }
    ThreadState* newThreadState(std::atomic<std::vector<uint8_t >*>* whichEPlan,
                                std::atomic<std::vector<uint8_t >*>* whichSPlan){
        return new ThreadState(threads.size(),setting,conn_,whichEPlan,whichSPlan);
    }
    void Load(void){
        DoWrite(true);
    }
    void Close(void){
        conn_->close(conn_, NULL);
        conn_ = NULL;
    }
    bool ReadOneKey(ThreadState *thread) {

        WT_CURSOR *cursor;
        int ret = thread->session->open_cursor(thread->session, uri_.c_str(), NULL, NULL, &cursor);
        if (ret != 0) {
            fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
            exit(1);
        }
        int found = 0;
        std::string key(allkeys.at(rand() % allkeys.size()));
        cursor->set_key(cursor, key.c_str());
        if (cursor->search(cursor) == 0) {
            found++;
            const char* val;
            ret = cursor->get_value(cursor, &val);
            assert(ret == 0);
        }
        cursor->close(cursor);
        return found > 0;
    }
    bool ReadOneKey(ThreadState *thread,std::string &key,std::string &value){
        WT_CURSOR *cursor;
        int ret = thread->session->open_cursor(thread->session, uri_.c_str(), NULL, NULL, &cursor);
        if (ret != 0) {
            fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
            exit(1);
        }
        int found = 0;

        cursor->set_key(cursor, key.c_str());
        if (cursor->search(cursor) == 0) {
            found++;
            const char* val;
            ret = cursor->get_value(cursor, &val);
            assert(ret == 0);
            value.assign(val);
        }
        cursor->close(cursor);
        return found > 0;
    }
    bool UpdateOneKey(ThreadState *thread) {

        std::string value;
        std::string &key = allkeys.at(rand() % allkeys.size());
        bool ret = ReadOneKey(thread,key,value);
        if (!ret)
            return false;

        return InsertOneKey(thread,key,value);
    }
    bool InsertOneKey(ThreadState *thread,std::string &key,std::string &value){
        WT_CURSOR *cursor;
        int ret = thread->session->open_cursor(thread->session, uri_.c_str(), NULL, NULL, &cursor);
        if (ret != 0) {
            fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
            exit(1);
        }
        cursor->set_key(cursor,key.c_str());
        cursor->set_value(cursor,value.c_str());
        ret = cursor->insert(cursor);
        if (ret != 0) {
            fprintf(stderr, "set error: %s\n", wiredtiger_strerror(ret));
            return false;
        }
        return true;

    }
    bool InsertOneKey(ThreadState *thread){
        WT_CURSOR *cursor;
        int ret = thread->session->open_cursor(thread->session, uri_.c_str(), NULL,NULL, &cursor);
        if (ret != 0) {
            fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
            exit(1);
        }
        std::string str;
        if ( !updateDataCq.try_pop(str)) {
            return false;
        }
        std::string key;
        std::string val;
        ret = getKeyAndValue(str,key,val);
        assert(ret > 0);
        allkeys.push_back(key);
        cursor->set_key(cursor, key.c_str());
        cursor->set_value(cursor,val.c_str());

        ret = cursor->insert(cursor);
        if (ret != 0) {
            fprintf(stderr, "set error: %s\n", wiredtiger_strerror(ret));
            return false;
        }
        cursor->close(cursor);
        return true;
    }
    void Open() {
        PrintHeader();
        PrintEnvironment();
        PrintWarnings();
#define SMALL_CACHE 10*1024*1024
        std::stringstream config;
        config.str("");
        if (!setting.FLAGS_use_existing_db) {
            config << "create";
        }
        if (setting.FLAGS_cache_size > 0)
            config << ",cache_size=" << setting.FLAGS_cache_size;
        config << ",log=(enabled,recover=on)";
        config << ",checkpoint=(log_size=64MB,wait=60)";
        /* TODO: Translate write_buffer_size - maybe it's chunk size?
           options.write_buffer_size = FLAGS_write_buffer_size;
           */
#ifndef SYMAS_CONFIG
        config << ",extensions=[libwiredtiger_snappy.so]";
#endif
        //config << ",verbose=[lsm]";
        Env::Default()->CreateDir(setting.FLAGS_db);
        wiredtiger_open(setting.FLAGS_db, NULL, config.str().c_str(), &conn_);
        assert(conn_ != NULL);

        WT_SESSION *session;
        conn_->open_session(conn_, NULL, NULL, &session);
        assert(session != NULL);

        char uri[100];
        snprintf(uri, sizeof(uri), "%s:dbbench_wt-%d",
                 setting.FLAGS_use_lsm ? "lsm" : "table", db_num_);
        uri_ = uri;

        if (!setting.FLAGS_use_existing_db) {
            // Create tuning options and create the data file
            config.str("");
            config << "key_format=S,value_format=S";
            config << ",columns=[p,val]";
            config << ",prefix_compression=true";
            config << ",checksum=off";

            if (setting.FLAGS_cache_size < SMALL_CACHE && setting.FLAGS_cache_size > 0) {
                config << ",internal_page_max=4kb";
                config << ",leaf_page_max=4kb";
                config << ",memory_page_max=" << setting.FLAGS_cache_size;
            } else {
                config << ",internal_page_max=16kb";
                config << ",leaf_page_max=16kb";
                if (setting.FLAGS_cache_size > 0) {
                    long memmax = setting.FLAGS_cache_size * 0.75;
                    // int memmax = setting.FLAGS_cache_size * 0.75;
                    config << ",memory_page_max=" << memmax;
                }
            }
            if (setting.FLAGS_use_lsm) {
                config << ",lsm=(";
                if (setting.FLAGS_cache_size > SMALL_CACHE)
                    config << ",chunk_size=" << setting.FLAGS_write_buffer_size;
                if (setting.FLAGS_bloom_bits > 0)
                    config << ",bloom_bit_count=" << setting.FLAGS_bloom_bits;
                else if (setting.FLAGS_bloom_bits == 0)
                    config << ",bloom=false";
                config << ")";
            }
#ifndef SYMAS_CONFIG
            config << ",block_compressor=snappy";
#endif
            fprintf(stderr, "Creating %s with config %s\n",uri_.c_str(), config.str().c_str());
            int ret = session->create(session, uri_.c_str(), config.str().c_str());
            if (ret != 0) {
                fprintf(stderr, "create error: %s\n", wiredtiger_strerror(ret));
                exit(1);
            }

            session->close(session, NULL);
        }
    }
    void DoWrite(bool seq) {
        std::cout << "DoWrite!" << std::endl;
        std::stringstream txn_config;
        txn_config.str("");
        txn_config << "isolation=snapshot";
        if (sync_)
            txn_config << ",sync=full";
        else
            txn_config << ",sync=none";
        WT_CURSOR *cursor;
        std::stringstream cur_config;
        cur_config.str("");
        cur_config << "overwrite";
        WT_SESSION *session;
        conn_->open_session(conn_, NULL, NULL, &session);
        int ret = session->open_cursor(session, uri_.c_str(), NULL, cur_config.str().c_str(), &cursor);
        if (ret != 0) {
            fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
            exit(1);
        }

        std::string str;
        long long recordnumber = 0;
        while(getline(loadFile, str)) {
            //寻找第二个和第三个\t
            std::string key;
            std::string val;
            ret = getKeyAndValue(str,key,val);
            assert(ret > 0);
            allkeys.push_back(key);
            cursor->set_key(cursor, key.c_str());
            cursor->set_value(cursor,val.c_str());
            //std::cout << "key:" << key << std::endl;
            int ret = cursor->insert(cursor);
            if (ret != 0) {
                fprintf(stderr, "set error: %s\n", wiredtiger_strerror(ret));
                exit(1);
            }
            recordnumber++;
            if (recordnumber % 100000 == 0)
                std::cout << "Record number: " << recordnumber << std::endl;
        }
        cursor->close(cursor);
        time_t now;
        struct tm *timenow;
        time(&now);
        timenow = localtime(&now);
        printf("recordnumber %lld,  time %s\n",recordnumber, asctime(timenow));
    }
};

#endif //TERARKDB_TEST_FRAMEWORK_BENCHMARK_H
