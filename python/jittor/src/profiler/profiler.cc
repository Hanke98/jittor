// ***************************************************************
// Copyright (c) 2021 Jittor. All Rights Reserved. 
// Maintainers: Dun Liang <randonlang@gmail.com>. 
// This file is subject to the terms and conditions defined in
// file 'LICENSE.txt', which is part of this source code package.
// ***************************************************************
#include <chrono>
#include <algorithm>
#include <sstream>
#include <iomanip>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#ifdef HAS_CUDA
#include <cuda_runtime.h>
#include "helper_cuda.h"
#endif
#include "misc/cuda_flags.h"
#include "profiler/profiler.h"
#include "op.h"
#include "fused_op.h"
#include "profiler/memory_checker.h"
#include "misc/deleter.h"

namespace jittor {

Profiler profiler;

DEFINE_FLAG(int, profiler_warmup, 0, "Profiler warmup.");
DEFINE_FLAG(int, profiler_rerun, 0, "Profiler rerun.");
DEFINE_FLAG(int, profiler_hide_relay, 0, "Profiler hide relayed op.");
DEFINE_FLAG_WITH_SETTER(int, profiler_enable, 0, "Enable profiler.");

void setter_profiler_enable(int value) {
    if (value)
        Profiler::start();
    else
        Profiler::stop();
}

Profiler::~Profiler() {
    if (profiler_enable) {
        Profiler::stop();
        Profiler::report();
    }
}

void Profiler::start(int64 warmup, int64 rerun) {
    if (warmup==0) warmup = profiler_warmup;
    if (rerun==0) rerun = profiler_rerun;
    profiler_enable = 1;
    profiler.records.clear();
    profiler.warmup = warmup;
    profiler.rerun = rerun;
}

void Profiler::stop() {
    profiler_enable = 0;
}

unique_ptr<MemoryChecker>* load_memory_checker(string name) {
    const char* msg = "";
    LOGvv << "Opening jit lib:" << name;
    #ifdef _WIN32
    void* handle = (void*)LoadLibrary(name.c_str());
    #elif defined(__linux__)
    void* handle = dlopen(name.c_str(), RTLD_LAZY | RTLD_DEEPBIND | RTLD_LOCAL);
    msg = dlerror();
    #else
    void* handle = dlopen(name.c_str(), RTLD_LAZY | RTLD_LOCAL);
    msg = dlerror();
    #endif

    CHECK(handle) << "Cannot open library" << name << ":" << msg;
    
    #ifdef _WIN32
    auto mm = (unique_ptr<MemoryChecker>*)GetProcAddress((HINSTANCE)handle, "memory_checker");
    #else
    //dlerror();
    auto mm = (unique_ptr<MemoryChecker>*)dlsym(handle, "memory_checker");
    msg = dlerror();
    #endif
    CHECK(!msg) << "Loading symbol memory_checker from" << name << "failed:" << msg;
    
    return mm;
}

EXTERN_LIB string _get_stack_info(Node* node);

static  string get_stack_info(Op* op) {
    string stack_info = "stack info:\n";
    if (string("fused") == op->name()) {
        auto fop = (FusedOp*)op;
        map<string, int> stacks;
        for (Op* op : fop->ops) {
            stacks[_get_stack_info(op)] = 1;
        }
        for (auto& kv : stacks) {
            stack_info += kv.first;
            stack_info += '\n';
        }
        if (trace_py_var >= 3) {
            std::stringstream ss;
            ss << "input from:\n";
            for (auto& vi : fop->vars) {
                if (vi.type == 0) {
                    auto v = vi.var;
                    ss << v->shape << ',' << v->dtype() << ',' << v->name << ',';
                    if (v->input())
                        ss << v->input()->name_ex() << ',' << _get_stack_info(v->input());
                    else
                        ss << _get_stack_info(v);
                    ss << '\n';
                }
            }
            stack_info += ss.str();
        }
        return stack_info;
    } else {
        stack_info += _get_stack_info(op);
        stack_info += '\n';
        if (trace_py_var >= 3) {
            std::stringstream ss;
            ss << "input from:\n";
            for (auto v : op->inputs()) {
                ss << v->shape << ',' << v->dtype() << ',' << v->name << ',';
                if (v->input())
                    ss << v->input()->name_ex() << ',' << _get_stack_info(v->input());
                else
                    ss << _get_stack_info(v);
                ss << '\n';
            }
            stack_info += ss.str();
        }
        return stack_info;
    }
}

void Profiler::record_and_run(
    jit_op_entry_t jit_entry,
    Op* op,
    const char* jit_key
) {
    if (!profiler_enable)
        jit_entry(op);
    else {
        auto ikey=jit_key_mapper.find(jit_key);
        const char* key = ikey==jit_key_mapper.end() ?
            jit_key : ikey->second.c_str();
        auto iter = profiler.records.find(key);
        uint64_t in, out, compute;
        op->statistics(in, out, compute);
        if (iter == profiler.records.end()) {
            profiler.records[key] = Info{
                0, 0, -1ull, 0, 
                0, 0, 0
            };
            iter = profiler.records.find(key);
            if (trace_py_var) {
                iter->second.stack_info = get_stack_info(op);
            }
        }
        bool is_fused = op->name() == string("fused");

        uint64* shape_time = nullptr;
        if (trace_py_var) {
            // record shape
            NanoVector shape;
            int64 num = 0;
            Op** ops = &op;
            int op_num = 1;
            if (is_fused) {
                ops = &(((FusedOp*)op)->ops[0]);
                op_num = ((FusedOp*)op)->ops.size();
            }
            for (int i=0; i<op_num; i++) {
                auto o = ops[i];
                for (auto v : o->inputs()) {
                    if (v->num > num) {
                        num = v->num;
                        shape = v->shape;
                    }
                }
                for (auto v : o->outputs()) {
                    if (v->num > num) {
                        num = v->num;
                        shape = v->shape;
                    }
                }
            }
            iter->second.shapes[shape].second += 1;
            shape_time = &iter->second.shapes[shape].first;
        }
        int loop = (is_fused && 
            ((FusedOp*)op)->get_loop_option("insert_profile_loop")) ? 10 : 0;
        int64_t warmup = profiler.warmup ? std::max(profiler.warmup>>loop, (int64_t)1) : 0;
        int64_t rerun = std::max((profiler.rerun+1)>>loop, (int64_t)1);
        // prevent relayed op being rerun
        auto warmup_bk = profiler.warmup;
        auto rerun_bk = profiler.rerun;
        profiler.warmup = profiler.rerun = 0;
        Deleter del([&]() {
            profiler.warmup = warmup_bk;
            profiler.rerun = rerun_bk;
        });
        
        for (int64_t i=0; i<warmup; i++) {
            jit_entry(op);
            #ifdef HAS_CUDA
            if (use_cuda)
                checkCudaErrors(cudaDeviceSynchronize());
            #endif
        }
        for (int64_t i=0; i<rerun; i++) {
            auto start = std::chrono::high_resolution_clock::now();
            jit_entry(op);
            #ifdef HAS_CUDA
            if (use_cuda)
                checkCudaErrors(cudaDeviceSynchronize());
            #endif
            auto finish = std::chrono::high_resolution_clock::now();
            auto total_ns =  (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(finish-start).count();
            // 24ns function call overhead
            total_ns = std::max((int64_t)1, total_ns-24);
            iter->second.update(loop, total_ns, in, out, compute);
            if (shape_time) shape_time[0] += total_ns;
            LOGvvvv << "Duration" << total_ns >> "ns running" << op;
        }
        if (is_fused && 
            ((FusedOp*)op)->get_loop_option("check_cache")) {
            auto fname = Op::get_filename_from_jit_key(key, ".so");
            unique_ptr<MemoryChecker>* mc = load_memory_checker(fname);
            iter->second.cache_info.reset(new CacheInfo(mc));
        }
    }
}

vector<vector<string>> Profiler::report(const string& sort_key) {
    vector<vector<string>> rep = {{"Name", "FileName", "Count", "TotalTime", "AvgTime", "MinTime", "MaxTime", "Input", "Output", "InOut", "Compute"}};
    vector<string> names, fnames;
    vector<vector<double>> info;
    vector<int> order;
    int sort_key_id = 0;
    for (; sort_key_id<(int)rep[0].size(); sort_key_id++)
        if (rep[0][sort_key_id] == sort_key)
            break;
    ASSERT(sort_key_id<(int)rep[0].size()) << "Key not supported:" << sort_key;
    double total_time = 0;
    double total_mem_access = 0;
    for (auto& kv : profiler.records) {
        auto& kinfo = kv.second;
        names.push_back(kv.first);
        fnames.push_back(Op::get_filename_from_jit_key(kv.first, ".cc"));
        if (kv.second.stack_info.size()) {
            fnames.back() += '\n';
            fnames.back() += kv.second.stack_info.c_str();
        }
        if (kv.second.shapes.size()) {
            // show shapes
            vector<pair<pair<uint64,uint64>,NanoVector>> shapes;
            shapes.reserve(kv.second.shapes.size());
            for (auto& kv2 : kv.second.shapes) {
                shapes.push_back(std::make_pair(kv2.second, kv2.first));
            }
            std::sort(shapes.begin(), shapes.end());
            std::stringstream ss;
            ss << "shapes:\n";
            for (int i=0; i<10; i++) {
                if (i>=shapes.size()) break;
                auto& sp = shapes[shapes.size() - i - 1];
                auto rate = sp.first.first * 100.0 / kinfo.time_total;
                ss << sp.second << ':' << sp.first.second << 
                    "("<< std::setprecision(3) << rate << "%), ";
            }
            if (shapes.size()>10)
                ss << "... total " << shapes.size() << '\n';
            fnames.back() += ss.str();
        }
        order.push_back(order.size());
        // do not count relay op time
        if (kv.first.find("relay") == string::npos) {
            total_time += kinfo.time_total;
            total_mem_access += kinfo.in_total + kinfo.out_total;
        }
        info.push_back({
            (double)kinfo.count, // Count
            (double)kinfo.time_total, // TotalTime
            (double)kinfo.time_total*1.0 / kinfo.count, // AvgTime
            (double)kinfo.time_min, // MinTime
            (double)kinfo.time_max, // MaxTime
            (double)kinfo.in_total*1e9 / kinfo.time_total, // Input
            (double)kinfo.out_total*1e9 / kinfo.time_total, // Output
            (double)(kinfo.in_total+kinfo.out_total)*1e9 / kinfo.time_total, // InOut
            (double)kinfo.compute_total*1e9 / kinfo.time_total, // Compute
        });
    }
    if (sort_key_id>=2)
        std::sort(order.begin(), order.end(), [&](int i, int j) {
            return info[i][sort_key_id-2] > info[j][sort_key_id-2];
        });
    else
        std::sort(order.begin(), order.end(), [&](int i, int j) {
            return names[i] > names[j];
        });
    std::stringstream ss;
    ss << "Profile result, sorted by " << sort_key << "\n"
        << "('it/s' represent number of iterations per sec)\n";
    uint w = 10, p=3;
    for (auto& s : rep[0]) {
        ss << std::setw(w) << s;
        if (s == "TotalTime")
            ss << std::setw(w) << "%,cum%";
    }
    ss << '\n';
    auto output_float = [&](const string& scale, int base, const string& suffix, double k) {
        ss << ' ' << std::setw(w-2-suffix.size());
        ss << std::setprecision(p);
        uint i=0;
        for (; i+1<scale.size(); i++) {
            if (k<base) break;
            k /= base;
        }
        ss << k << scale[i];
        ss << suffix;
    };
    ss << "Total time:";
    output_float("num ", 1000, "s", total_time);
    ss << '\n';
    ss << "Total Memory Access:";
    output_float(" KMG", 1024, "B", total_mem_access);
    ss << '\n';
    double cum_time = 0;
    for (auto i : order) {
        auto& name = names[i];
        auto is_relay = name.find("relay") != string::npos;
        if (is_relay && profiler_hide_relay)
            continue;
        auto& fname = fnames[i];
        rep.push_back({name, fname});
        ss << std::setw(w) << name;
        if (name.size() >= w-1)
            ss << "\n" << std::setw(w) << " ";
        ss << std::setw(w) << fname;
        if (fname.size() >= w-1)
            ss << "\n" << std::setw(w*2) << " ";
        for (uint j=0; j<info[i].size(); j++) {
            auto k = info[i][j];
            if (j==0)
                ss << ' ' << std::setw(w-1) << k;
            else if (j<=4) {
                // output time
                output_float("num ", 1000, "s", k);
                // output total ratio
                if (j == 1) {
                    // do not count relay op time
                    if (is_relay)
                        k = 0;
                    cum_time += k;
                    ss << '(' << std::setw(3)
                        << std::setprecision(p) << k / total_time * 100 << "%,"
                        << std::setw(3)
                        << std::setprecision(p) << cum_time / total_time * 100 << "%)";
                }
            } else if (j<=7) {
                // output thoughtput
                output_float(" KMG", 1024, "B/s", k);
            } else {
                // computation thoughtput
                output_float(" KMG", 1000, "it/s", k);
            }
            std::stringstream s2;
            s2 << k;
            rep.back().push_back(s2.str());
        }
        ss << '\n';
    }
    LOGi >> '\n' >> ss.str() >> '\n';

    //cache rep
    // TODO: report_cache sort_key
    vector<vector<string>> rep_cache = report_cache("CheckTimes");
    if (rep_cache.size() > 1)
        rep.insert(rep.end(), rep_cache.begin(), rep_cache.end());
    return rep;
}

vector<vector<string>> Profiler::report_cache(const string& sort_key) {
    vector<vector<string>> rep = {{"Name", "FileName", "CheckTimes", "TLBMissRate"}};
    vector<string> names, fnames;
    vector<vector<double>> info;
    vector<vector<int>> int_info;
    vector<int> order;
    int sort_key_id = 0;
    for (; sort_key_id<(int)rep[0].size(); sort_key_id++)
        if (rep[0][sort_key_id] == sort_key)
            break;
    ASSERT(sort_key_id<(int)rep[0].size()) << "Key not supported:" << sort_key;
    sort_key_id--;
    for (auto& kv : profiler.records) {
        if (!kv.second.cache_info)
            continue;
        names.push_back(kv.first);
        fnames.push_back(Op::get_filename_from_jit_key(kv.first, ".cc"));
        CacheInfo& kinfo = *kv.second.cache_info;
        order.push_back(order.size());
        vector<double> one_info = {(double)kinfo.check_times, ((double)kinfo.tlb_miss_times) / kinfo.check_times};
        vector<int> one_int_info = {(int)kinfo.check_times, (int)kinfo.tlb_miss_times};
        for (int i = 0; i < (int)kinfo.cache_miss_times.size(); ++i) {
            if ((int)rep[0].size() < 4 + i + 1) {
                std::stringstream ss;
                ss << "L" << i + 1 << "MissRate";
                rep[0].push_back(ss.str());
            }
            one_info.push_back(((double)kinfo.cache_miss_times[i]) / kinfo.check_times);
            one_int_info.push_back((int)kinfo.cache_miss_times[i]);
        }
        info.push_back(one_info);
        int_info.push_back(one_int_info);
    }
    if (sort_key_id>0)
        std::sort(order.begin(), order.end(), [&](int i, int j) {
            return info[i][sort_key_id-1] > info[j][sort_key_id-1];
        });
    else
        std::sort(order.begin(), order.end(), [&](int i, int j) {
            return names[i] > names[j];
        });
    std::stringstream ss;
    ss << "Memory profile result, sorted by " << sort_key << "\n";
    uint w = 15;
    for (auto& s : rep[0])
        ss << std::setw(w) << s;
    ss << '\n';
    for (auto i : order) {
        auto& name = names[i];
        auto& fname = fnames[i];
        rep.push_back({name, fname});
        ss << std::setw(w) << name;
        if (name.size() >= w-1)
            ss << "\n" << std::setw(w) << " ";
        ss << std::setw(w) << fname;
        if (fname.size() >= w-1)
            ss << "\n" << std::setw(w*2) << " ";
        for (uint j=0; j<info[i].size(); j++) {
            auto k = info[i][j];
            auto int_k = int_info[i][j];
            if (j == 0)
                ss << ' ' << std::setw(w-1) << (int)k;
            else
                ss << ' ' << std::setw(w-2) << k * 100 << "%";

            std::stringstream s2;
            s2 << int_k;
            rep.back().push_back(s2.str());
        }
        ss << '\n';
    }
    LOGi >> '\n' >> ss.str() >> '\n';

    return rep;
}

} // jittor