/*
    TickTock is an open-source Time Series Database for your metrics.
    Copyright (C) 2020-2021  Yongtao You (yongtao.you@gmail.com),
    Yi Lin (ylin30@gmail.com), and Yalei Wang (wang_yalei@yahoo.com).

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include "json.h"
#include "stop.h"
#include "task.h"


namespace tt
{


// the first char must be unique
#define PIPE_CMD_REBALANCE_CONN     "b\n"
#define PIPE_CMD_NEW_CONN           "c\n"
#define PIPE_CMD_DISCONNECT_CONN    "d\n"
#define PIPE_CMD_FLUSH_APPEND_LOG   "f\n"
#define PIPE_CMD_CLOSE_APPEND_LOG   "g\n"
#define PIPE_CMD_SET_STOPPED        "s\n"

#define DONT_FORWARD                "don't forward\n"

class TcpServer;
class TcpListener;

#define TCS_NONE        0x00000000
#define TCS_REGISTERED  0x00000001
#define TCS_ERROR       0x00000002
#define TCS_CLOSED      0x00000004

#define INVALID_WORKER_ID   (-1)


class TcpConnection : public Recyclable
{
public:
    int fd; // socket file-descriptor
    TcpServer *server;
    TcpListener *listener;
    bool forward;

    std::atomic<int> worker_id;
    std::atomic<unsigned int> state; // TCS_xxx

    char *buff;
    int offset;

    // last time there was any activity on this connection,
    // used to decide if we should close the connection or not;
    std::chrono::steady_clock::time_point last_contact;

    TcpConnection()
    {
        init();
    }

protected:

    void init()
    {
        fd = -1;
        server = nullptr;
        listener = nullptr;
        worker_id = INVALID_WORKER_ID;
        state = TCS_NONE;
        buff = nullptr;
        offset = 0;
        forward = g_cluster_enabled;
        last_contact = std::chrono::steady_clock::now();
    }
};


class PipeReader
{
public:
    PipeReader(int fd);
    char *read_pipe();
    const char *c_str(char *buff, size_t size) const;

private:
    int m_fd;
    char m_buff[64];
    int m_index;
    char m_buff2[64];
};


/* There are 2 types of TcpListeners.
 *
 * Level 0 listener receives (and accept()) new incoming HTTP connections.
 * It will then pass the new connection to one of the least busy Level 1
 * listeners.
 *
 * Level 1 listeners will receive HTTP requests from a number of
 * connections assigned to it by the Level 0 listener. It will then
 * pass the HTTP request to one of its responders to process & response.
 * The number of responders each Level 1 listener has is determined by
 * the http.responders.per.listener config.
 *
 * There is exactly 1 Level 0 listener; and there are a number of
 * Level 1 listeners. The exact number of Level 1 listeners can be
 * set in the config (http.listener.count).
 *
 * Level 0 listener is implemented in listener0();
 * Level 1 listener is implemented in listener1();
 */
class TcpListener : public Stoppable
{
public:
    TcpListener();             // this c'tor will not create listening thread
    TcpListener(TcpServer *server, int fd, size_t max_conn);  // level 0
    TcpListener(TcpServer *server, int fd, size_t max_conn, int id);  // level 1
    virtual ~TcpListener();

    void shutdown(ShutdownRequest request = ShutdownRequest::ASAP);
    void wait(size_t timeout_secs); // BLOCKING CALL!
    void close_conns();

    inline bool is_stopped() const
    {
        return Stoppable::is_stopped() && m_responders.is_stopped();
    }

    void instruct(const char *instruction, int size);

    static int get_active_conn_count();

    inline size_t get_pending_task_count() const
    {
        m_responders.get_pending_task_count();
    }

    inline int get_total_task_count(size_t counts[], int size) const
    {
        return m_responders.get_total_task_count(counts, size);
    }

    inline int get_socket_fd() const
    {
        return m_socket_fd;
    }

private:
    bool init(int socket_fd);
    void listener0();   // level 0: handle tcp connections
    void listener1();   // level 1: handle http traffic
    bool register_with_epoll(int fd);
    bool deregister_with_epoll(int fd);

    void rebalance0();
    void rebalance1();
    void disconnect();
    void flush_append_log();
    void close_append_log();

    void new_conn0();
    void new_conn2(int fd);
    void close_conn(int fd);

    TcpConnection *get_conn(int fd);
    TcpConnection *get_or_create_conn(int fd);

    void send_response(TcpConnection *conn);

    static void write_pipe(int fd, const char *msg);

    int m_id;
    TcpServer *m_server;       // the http server we belong to

    // If not nullptr, this points to the level 1 listener with
    // least of connections. And we should move one of our connections
    // to it.
    std::atomic<TcpListener*> m_least_conn_listener;
    std::atomic<TcpConnection*> m_conn_in_transit;

    size_t m_max_conns;         // max number of connections allowed
    size_t m_max_events;        // max number of epoll events per epoll_wait()
    size_t m_conn_timeout_secs; // timeout for idle connections

    // remember fd to responder assignment so that we can assign all pieces of
    // a broken-up request to the same worker thread. Listener will add the
    // mapping before handing over the task to responder, and responder will
    // remove the mapping just before sending out the response.
    // Note: This map is read/written by the listener thread alone! Responders
    // are passed the TcpConnection* which they can update, but they will
    // not search nor update this map.
    // TODO: this can be a simple array of TcpConnection indiced by fd
    std::map<int,TcpConnection*> m_conn_map;   // from fd to TcpConnection

    static std::mutex m_lock;
    static std::map<int,TcpConnection*> m_all_conn_map;   // from fd to TcpConnection

    TcpConnection *m_live_conns; // list (double-linked) of live connections
    TcpConnection *m_free_conns; // stack of available connections
    TcpConnection *m_conns;    // TODO: is this needed? save this so we can delete it

    int m_socket_fd;            // main socket we listen on
    int m_epoll_fd;             // epoll socket for the event loop
    int m_pipe_fds[2];          // self-pipe trick to wake up epoll_wait()

    // this needs to be a signed integer, because connections can be opened in
    // one thread and closed in another. Luckily we only care about the total.
    //int m_stats_active_conn_count;

    TaskScheduler m_responders; // threads to handle http requests
    std::thread m_listener;     // the thread that goes into the event loop
};


class TcpServer : public Stoppable
{
public:
    TcpServer();
    TcpServer(int listener_count);
    virtual ~TcpServer();

    void init();
    bool start(int port);
    void shutdown(ShutdownRequest request = ShutdownRequest::ASAP);
    void wait(size_t timeout_secs); // BLOCKING CALL!
    void close_conns();
    bool is_stopped() const;
    void instruct0(const char *instruction, int size);

    TcpListener* next_listener();  // listener to receive new connection
    void get_level1_listeners(std::vector<TcpListener*>& listeners) const;

    size_t get_active_conn_count() const;
    size_t get_pending_task_count() const;
    int get_total_task_count(size_t counts[], int size) const;

protected:
    virtual TcpConnection *create_conn() const;
    virtual Task get_recv_data_task(TcpConnection *conn) const;

    // task func
    static bool recv_tcp_data(TaskData& data);

private:
    friend class TcpListener;
    static bool set_flags(int fd, int flags);
    static bool process_data(TcpConnection *conn, char *data, int len);
    static void send_response(int fd, char *content, int len);

    void instruct1(const char *instruction, int size);

    TcpListener *get_least_conn_listener() const;
    TcpListener *get_most_conn_listener() const;

    int m_next_listener;        // used to distribute new connections
    size_t m_listener_count;    // number of listeners (max 2 as hard-coded below)
    TcpListener **m_listeners; // threads that go into the event loop

    size_t m_max_conns_per_listener;
    int m_socket_fd;            // main socket we listen on
};


}
