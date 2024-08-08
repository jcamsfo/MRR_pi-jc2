//
// Created by jonathan on 9/10/20.
//

#ifndef COMMS_H
#define COMMS_H


#ifdef _WINDOWS
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <netdb.h>
typedef int SOCKET;
#endif

#include <thread>
#include <string>
#include <deque>
#include <mutex>
#include <list>
#include <vector>
#include <condition_variable>
#include <chrono>
#include <map>
#include <atomic>

using namespace std;

string load_image(const string & raw_filename);

typedef std::chrono::steady_clock SteadyClock;
typedef std::chrono::duration<double> Seconds;

enum ConnectError {
    PENDING,
    SUCCESS,
    ADDR_INFO_ERROR,
    FAILED_TO_CONNECT,
    SERVER_DISCONNECTED,
    RECEIVE_POLL_ERROR,
    SEND_POLL_ERROR,
    SEND_COUNT_FAILURE,
    FAILED_TO_BIND,
    LISTEN_FAILURE,
    BLOCKING_FAILURE,
    CONNECTION_FAILURE,
    CREATE_SOCKET_FAILURE,
    SEND_TIMEOUT,
};

enum MessageState {
    WAITING,
    WAITING_FOR_HEADER,
    STARTED,
    ONGOING
};

struct MessageData {
    static const int header_size;
    
    enum MessageType {
        NONE,
        DISPLAY_NOW,
        IMAGE,
        START_TIMER,
        ACK
    };
    
    MessageType message_type;
    string image_name;
    string image_data;
    std::atomic<int> use_count;
    bool auto_delete = true;
    
    MessageData(MessageType message_type);
    MessageData(MessageType message_type, const string & image_name);
    MessageData(MessageType message_type, const string & image_name, const string & image_data);
    MessageData(MessageType message_type, int name_length, long image_length, string & buffer);
    string serialize_header() const;
    static MessageData * deserialize(string & buffer, MessageState message_state);
};

struct Connection {
    SOCKET sock_fd = 0;
    bool keep_going_flag = true;
    bool local = true;
    thread* send_thread = nullptr;
    thread* receive_thread = nullptr;
    string id;
    mutex send_values_mutex;
    // keeps the list of pending key/values to send
    deque<MessageData *> send_values;
    string received_so_far;

    void stop();
    MessageData* next_send();
    void send(MessageData * message_data);
};

struct Waiter {
    mutex cv_mtx;
    condition_variable cv;
    
    void wait();
    cv_status wait_for(const Seconds& rel_time);
    void notify();
};

class Comm; // forward reference
typedef Comm * (*CommFactory)();

class Comm {
public:
    Comm();
    ~Comm();

    enum Role {
        CLIENT,
        SERVER,
    };

    enum RemoteConnectionResult {
        OK,
        FAIL,
        CONTINUE
    };
    
    // note: mixing BLOCKING and NON_BLOCKING calls to the same ip/port
    // may result in unpredictable behavior
    
    enum BlockType {
        BLOCKING,
        NON_BLOCKING
    };
    
    // returns false if instance is already connected or connecting
    bool connect(Role role, const string & ip_address, const string & port);
    void set_waiter(Waiter * waiter);
    ConnectError connect_result();
    //  caller must dispose of the pointer
    MessageData * next_received();
    void disconnect();
    ConnectError send(MessageData * message_data, BlockType block=NON_BLOCKING);
    void send_display_now(const string & image_name = "");
    void send_image(const string & image_name, const string & image_data);
    void send_start_timer();
    void send_ack(const string & image_name);
    const string & ip() const;
    const string & port() const;
    
    static Comm * start_server(Waiter * waiter, int argc, char* argv[], CommFactory = nullptr);
    static list<Comm *> start_clients(Waiter * waiter, int argc, char* argv[], CommFactory = nullptr);
    static const string default_port;

protected:
    // only for SERVER roles
    virtual bool allow_new_connection(const sockaddr_storage & sin_addr, socklen_t sin_size);
    // only for SERVER roles
    virtual void got_new_connection(const  sockaddr_storage& sin_addr, socklen_t sin_size);

private:
    void execute_connect(Role pending_role, const string & ip_address, const string & port);
    void execute_send(Connection * remote_connection);
    void execute_receive(Connection * remote_connection);
    void set_connect_error(ConnectError connect_error);
    void sendAndReceive(Connection * remote_connection);
    // returns false if failed; if true sock_fd = new socket
    bool create_socket(const string & ip_address, const string & port, SOCKET & sock_fd);
    bool is_server() const;
    void close_all();
    void close_one(Connection* remote_connection);
    void add_connection(Connection * remote_connection);
    RemoteConnectionResult init_remote_connection(Connection* remote_connection, SOCKET candidate_fd);
    ConnectError send_one(Connection * remote_connection, MessageData * message_data);

private:
    string ip_address;
    string ip_port;
    ConnectError connect_error = ConnectError::PENDING;
    thread * connect_thread = nullptr;
    mutex connect_result_mutex;
    mutex received_values_mutex;
    mutex remote_connections_mutex;
    Role role = Comm::Role::CLIENT;
    Connection local_connection;
    Waiter * waiter = nullptr;
    MessageState message_state = MessageState::WAITING;
    SteadyClock::time_point receive_begin;

    // keeps the list of incoming values
    deque<MessageData *> received_values;

    list<Connection *> remote_connections;
    list<Connection*> deleted_remote_connections;
};

struct SD {
    static const long warm_up = 300;
    
    long count = 0;
    SteadyClock::time_point last;
    double mean = 0;
    double sum_squares = 0;
    mutex sd_mutex;
    deque<double> sd_q;
    
    double increment(const SteadyClock::time_point & current);
    double increment(const SteadyClock::time_point & current, const SteadyClock::time_point & previous);
    double increment(const Seconds & seconds);
    void dump(ofstream & out, const string & label);
};

typedef void (*DisplayFunction)(const string & image_data);

struct Display {
    map<string, MessageData *> pending_images;
    deque<string> images_to_display;
    mutex queues_mutex;
    bool keep_going = true;
    
    long video_buffer_size;
    thread * display_thread = nullptr;
    long display_q_count = 0;
    long display_count = 0;
    long pending_q_count = 0;
    long name_not_found_count = 0;
    DisplayFunction display_function = nullptr;
    SD fwrite_sd;
    SD display_sd;
    
    void queue_image_for_display(MessageData * message_data);
    void dump(ofstream & out);
    void image_should_be_displayed(string image_name);
    void execute_display();
    void set_display_function(DisplayFunction display_function);
};



#endif //COMMS_H
