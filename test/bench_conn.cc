// Copyright (c) 2014-2018, NetApp, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.


#include <arpa/inet.h>
#include <cstdint>
#include <fcntl.h>
#include <libgen.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <benchmark/benchmark.h>
#include <quant/quant.h>
#include <warpcore/warpcore.h>


static struct w_engine * w;
static struct q_conn *cc, *sc;


static inline uint64_t io(const uint64_t len)
{
    // reserve a new stream
    struct q_stream * const cs = q_rsv_stream(cc, true);
    if (unlikely(cs == nullptr))
        return 0;

    // allocate buffers to transmit a packet
    struct w_iov_sq o = w_iov_sq_initializer(o);
    q_alloc(w, &o, len);

    // send the data
    q_write(cs, &o, true);

    // read the data
    struct w_iov_sq i = w_iov_sq_initializer(i);
    struct q_stream * const ss = q_read(sc, &i, true);
    if (likely(ss)) {
        q_readall_stream(ss, &i);
        q_close_stream(ss);
    }
    q_close_stream(cs);

    const uint64_t ilen = w_iov_sq_len(&i);
    q_free(&i);
    q_free(&o);

    return ilen;
}


static void BM_conn(benchmark::State & state)
{
    const auto len = uint64_t(state.range(0));
    for (auto _ : state) {
        const uint64_t ilen = io(len);
        if (ilen != len) {
            state.SkipWithError("error");
            return;
        }
    }
    state.SetBytesProcessed(int64_t(state.iterations() * len)); // NOLINT
}


BENCHMARK(BM_conn)->RangeMultiplier(2)->Range(1024, 1024 * 1024 * 2)
    // ->Unit(benchmark::kMillisecond)
    ;


// BENCHMARK_MAIN()

int main(int argc __attribute__((unused)), char ** argv)
{
#ifndef NDEBUG
    util_dlevel = DBG; // default to maximum compiled-in verbosity
#endif

    // init
    const int cwd = open(".", O_CLOEXEC);
    ensure(cwd != -1, "cannot open");
    ensure(chdir(dirname(argv[0])) == 0, "cannot chdir");
    const struct q_conf conf = {nullptr, "dummy.crt", "dummy.key"};
    w = q_init("lo"
#ifndef __linux__
               "0"
#endif
               ,
               &conf);
    ensure(fchdir(cwd) == 0, "cannot fchdir");

    // bind server socket
    q_bind(w, 55555);

    // connect to server
    struct sockaddr_in sip = {};
    sip.sin_family = AF_INET;
    sip.sin_port = htons(55555);
    sip.sin_addr.s_addr = inet_addr("127.0.0.1");
    cc = q_connect(w, &sip, "localhost", nullptr, nullptr, true, nullptr);
    ensure(cc, "is zero");

    // accept connection
    sc = q_accept(nullptr);
    ensure(sc, "is zero");

    benchmark::RunSpecifiedBenchmarks();

    // close connections
    q_close(cc);
    q_close(sc);
    q_cleanup(w);
}
