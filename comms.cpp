//
// Created by jonathan on 9/10/20.
//


#ifndef _WINDOWS
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/poll.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <iostream>
#include <thread>
#include <fcntl.h>
#include <streambuf>
#include <fstream>

#include <regex>
#include <algorithm>
#include <cmath>

#include "comms.h"

using namespace std;

#ifdef _WINDOWS
#define ssize_t SSIZE_T 
#endif

int const MessageData::header_size = 6;  // 1 for type, 1 for name length, 4 for image length
string const Comm::default_port("5569");

string load_image(const string & raw_filename) {
    ifstream input_stream(raw_filename, ios::binary);
    ostringstream string_stream;
    string_stream << input_stream.rdbuf();
    return string_stream.str();
}

string MessageData::serialize_header() const {
    string header;
    
    header.push_back(static_cast<char>(this->message_type));
    
    auto image_name_length = this->image_name.size();
    if (image_name_length > 255) {
        image_name_length = 255;
    }
    header.push_back(static_cast<unsigned char>(image_name_length));
    
    uint32_t image_size = (uint32_t) this->image_data.size();
    header.append(reinterpret_cast<char *>(&image_size), sizeof(image_size));
    
    if (image_name_length != 0) {
        header.append(image_name);
    }
    
    // cout << "header: sz:" << header.size() << " type:" << static_cast<int>(header[0]) << endl;
    return header;
}

MessageData::MessageData(MessageType message_type) {
    this->message_type = message_type;
}

MessageData::MessageData(MessageType message_type, const string & image_name) {
    this->message_type = message_type;
    this->image_name = image_name;
}

MessageData::MessageData(MessageType message_type, const string & image_name, const string & image_data) {
    this->message_type = message_type;
    this->image_name = image_name;
    this->image_data = image_data;
}

MessageData::MessageData(MessageType message_type, int name_length, long image_length, string & buffer) {
    this->message_type = message_type;
    if (name_length > 0) {
        this->image_name.append(&buffer[header_size], name_length);
    }
    if (image_length > 0) {
        this->image_data.append(&buffer[header_size + name_length], image_length);
    }
}

MessageData * MessageData::deserialize(string & buffer, MessageState message_state) {
    if (buffer.size() < MessageData::header_size) {
        return nullptr;
    }
    
    MessageType message_type = static_cast<MessageType>(buffer[0]);
    int name_length = static_cast<int>(buffer[1]);
    uint32_t image_length = *(reinterpret_cast<uint32_t *>(&buffer[2]));
    
    if (message_state == MessageState::STARTED) {
        cout << "got buffer mt:" << message_type << " nl:" << name_length << " il:" << image_length << endl;
    }
    
    if (buffer.size() < name_length + image_length + header_size) {
        return nullptr;
    }
    
    auto message_data = new MessageData(message_type, name_length, image_length, buffer);
    buffer.erase(0, name_length + image_length + header_size);
    
    return message_data;
}

void Connection::stop() {
    keep_going_flag = false;

    if (send_thread) {
        send_thread->join();
        delete send_thread;
        send_thread = nullptr;
    }
    if (receive_thread) {
        receive_thread->join();
        delete receive_thread;
        receive_thread = nullptr;
    }
}

void Connection::send(MessageData * message_data) {
    lock_guard<mutex> guard(this->send_values_mutex);
    send_values.emplace_back(message_data);
}

MessageData* Connection::next_send() {
    lock_guard<mutex> guard(this->send_values_mutex);
    if (send_values.empty()) {
        return nullptr;
    }

    MessageData* message_data = send_values.front();
    send_values.pop_front();

    return message_data;
};

void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((sockaddr_in*) sa)->sin_addr);
    }

    return &(((sockaddr_in6*) sa)->sin6_addr);
}

bool set_socket_blocking_enabled(SOCKET sock_fd, bool blocking) {
    // https://stackoverflow.com/questions/1543466/how-do-i-change-a-tcp-socket-to-be-non-blocking

    if (sock_fd < 0) {
        return false;
    }

#ifdef _WIN32
    unsigned long mode = blocking ? 0 : 1;
    return (ioctlsocket(sock_fd, FIONBIO, &mode) == 0) ? true : false;
#else
    int flags = fcntl(sock_fd, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }

    flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return fcntl(sock_fd, F_SETFL, flags) == 0;
#endif
}

void cross_close(SOCKET sockfd) {
#ifdef _WINDOWS
    closesocket(sockfd);
#else
    ::close(sockfd);
#endif
}

int cross_poll(pollfd* ufds, unsigned int nfds, int timeout) {
#ifdef _WINDOWS
    return WSAPoll(ufds, nfds, timeout);
#else
    return poll(ufds, nfds, timeout);
#endif
}

void Waiter::wait() {
    unique_lock<mutex> lock(this->cv_mtx);
    this->cv.wait(lock);
}

cv_status Waiter::wait_for(const Seconds & rel_time) {
    unique_lock<mutex> lock(this->cv_mtx);
    return this->cv.wait_for(lock, rel_time);
}

void Waiter::notify() {
    unique_lock<mutex> lock(this->cv_mtx);
    this->cv.notify_all();
}

Comm::Comm() {
#ifdef _WINDOWS
    WSADATA wsa_data;
    // Initialize Winsock
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) {
        cerr << "WSAStartup failed:" << result << endl;
    }
#endif
}

Comm::~Comm() {
#ifdef _WINDOWS
    WSACleanup();
#endif

    close_all();
}

list<Comm *> Comm::start_clients(Waiter * waiter, int argc, char* argv[], CommFactory comm_factory) {
    list<Comm *> comms;
    list<string> port_numbers;
    list<string> ip_addresses;
    
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i],"-p") == 0) {
            port_numbers.push_back(argv[i+1]);
        }
        else if (strcmp(argv[i],"-i") == 0) {
            ip_addresses.push_back(argv[i+1]);
        }
    }
    
    if (port_numbers.size() != ip_addresses.size()) {
        cout << "the count of port_numbers (" << port_numbers.size() << ") does not match the number of ip_addresses (" << ip_addresses.size() << ")" << endl;
    }
    
    if (port_numbers.size() == 0) {
        // defaults
        port_numbers.push_back(Comm::default_port);
        ip_addresses.push_back("127.0.0.1");
    }
    
    std::list<string>::iterator port_it = port_numbers.begin();
    std::list<string>::iterator ip_it = ip_addresses.begin();
    while(port_it != port_numbers.end()) {
        Comm * comm = comm_factory ? comm_factory() : new Comm();
        comm->set_waiter(waiter);
        comm->connect(Comm::Role::CLIENT, *ip_it, *port_it);
        comms.push_back(comm);
        port_it++;
        ip_it++;
    }
    
    bool all_connected = false;
    while (!all_connected) {
        all_connected = true;
        for (auto & comm : comms) {
            if (comm->connect_result() == ConnectError::PENDING) {
                all_connected = false;
                break;
            }
        }
        if (!all_connected) {
            this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    for (auto comm : comms) {
        if (comm->connect_result() != ConnectError::SUCCESS) {
            for (auto del_comm : comms) {
                delete del_comm;
            }
            comms.clear();
            break;
        }
    }
    
    return comms;
}

Comm * Comm::start_server(Waiter * waiter, int argc, char* argv[], CommFactory comm_factory) {
    string port_number(Comm::default_port);
    
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i],"-p") == 0) {
            port_number = argv[i+1];
        }
    }
    
    Comm * comm = comm_factory ? comm_factory() : new Comm();
    comm->set_waiter(waiter);
    comm->connect(Comm::Role::SERVER, "", port_number);
    
    while (comm->connect_result() == ConnectError::PENDING) {
        this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (comm->connect_result() != ConnectError::SUCCESS) {
        delete comm;
        return nullptr;
    }
    
    return comm;
}

void Comm::set_connect_error(ConnectError a_connect_error) {
    lock_guard<mutex> guard(this->connect_result_mutex);
    this->connect_error = a_connect_error;
}

void Comm::sendAndReceive(Connection * remote_connection) {
    local_connection.send_thread = new thread(&Comm::execute_send, this, remote_connection);
    local_connection.receive_thread = new thread(&Comm::execute_receive, this, remote_connection);
}

bool Comm::create_socket(const string & ip_address, const string & port, SOCKET & sock_fd) {
    if (this->role == Role::SERVER) {
        sock_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd < 0) {
            cout << "opening socket" << endl;
            return false;
        }

        sockaddr_in socket_addr;
        memset(&socket_addr, 0, sizeof(socket_addr));
        socket_addr.sin_family = AF_INET;
        socket_addr.sin_addr.s_addr = INADDR_ANY;
        socket_addr.sin_port = htons(stoi(port));

        if (::bind(sock_fd, (struct sockaddr*) &socket_addr, (int) sizeof(socket_addr)) == -1) {
            cross_close(sock_fd);
            cerr << "server bind failed (possibly the port is in use) or " << gai_strerror(errno) << endl;
            return false;
        }

        cout << "server listening at localhost:" << port << endl;
        return true;
    }
    else {
        addrinfo hints;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        int addrinfo_result;
        addrinfo* servinfo;
        if ((addrinfo_result = getaddrinfo(ip_address.c_str(), port.c_str(), &hints, &servinfo)) != 0) {
            cerr << "getaddrinfo: " << gai_strerror(addrinfo_result) << endl;
            set_connect_error(ADDR_INFO_ERROR);
            return false;
        }

        // loop through all the results and bind (server) or connect (client) to the first we can
        addrinfo* p;
        for (p = servinfo; p != nullptr; p = p->ai_next) {
            cout << "family:" << p->ai_family << " type:" << p->ai_socktype << " protocol:" << p->ai_protocol << " addr:" << p->ai_addr << endl;
            if ((sock_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
                cout << "unable to create socket, try another candidate" << endl;
                continue;
            }

            int connectResult = ::connect(sock_fd, p->ai_addr, (int) p->ai_addrlen);
            if (connectResult == -1) {
                cross_close(sock_fd);
                cerr << "connection attempt failed " << gai_strerror(errno) << endl;
                continue;
            }

            break;
        }

        if (p == nullptr) {
            freeaddrinfo(servinfo); // all done with this structure
            if (role == Role::CLIENT) {
                cerr << "client: failed to connect" << endl;
                set_connect_error(FAILED_TO_CONNECT);
            }
            else {
                cerr << "server: failed to bind" << endl;
                set_connect_error(FAILED_TO_BIND);
            }
            return false;
        }

        char info_buffer[INET6_ADDRSTRLEN];
        inet_ntop(p->ai_family, get_in_addr((struct sockaddr*)p->ai_addr), info_buffer, sizeof info_buffer);
        cout << "client: connecting to " << info_buffer << endl;

        freeaddrinfo(servinfo); // all done with this structure
        return true;
    }
}

bool Comm::is_server() const {
    if (role == Role::SERVER) {
        return true;
    }

    return false;
}

const string & Comm::ip() const {
    return this->ip_address;
}

const string & Comm::port() const {
    return this->ip_port;
}

bool Comm::allow_new_connection(const sockaddr_storage& sin_addr, socklen_t sin_size) {
    // only allow one connection at a time
    lock_guard<mutex> guard(this->remote_connections_mutex);
    return (this->remote_connections.empty());
}

void Comm::got_new_connection(const sockaddr_storage& sin_addr, socklen_t sin_size) {
}

void Comm::add_connection(Connection * remote_connection) {
    lock_guard<mutex> guard(this->remote_connections_mutex);
    this->remote_connections.emplace_back(remote_connection);
}

Comm::RemoteConnectionResult Comm::init_remote_connection(Connection* remote_connection, SOCKET candidate_fd) {

    remote_connection->local = false;
    remote_connection->sock_fd = candidate_fd;
    add_connection(remote_connection);
    set_connect_error(ConnectError::SUCCESS);
    sendAndReceive(remote_connection);

    return RemoteConnectionResult::OK;
}

void Comm::execute_connect(Role role, const string & ip_address, const string & port) {
    // see https://beej.us/guide/bgnet/html/#a-simple-stream-client

    this->role = role;

    if (!create_socket(ip_address, port, local_connection.sock_fd)) {
        set_connect_error(CREATE_SOCKET_FAILURE);
        local_connection.keep_going_flag = false;
        return;
    }

    if (is_server()) {
        bool blocking_result = set_socket_blocking_enabled(local_connection.sock_fd, false);
        if (!blocking_result) {
            set_connect_error(BLOCKING_FAILURE);
            local_connection.keep_going_flag = false;
            return;
        }
    }

    if (local_connection.keep_going_flag) {
        if (is_server()) {
            int backlog = 2;  // how many pending connections queue will hold
            // listen seems to be non-blocking
            if (listen(local_connection.sock_fd, backlog) == -1) {
                cout << "listen failure" << endl;
                set_connect_error(LISTEN_FAILURE);
                local_connection.keep_going_flag = false;
                return;
            }

            // at this point there's nothing connected yet
            
            while (local_connection.keep_going_flag) {
                struct sockaddr_storage client_addr; // connector's address information
                socklen_t sin_size = sizeof client_addr;
                SOCKET candidate_fd = accept(local_connection.sock_fd, (struct sockaddr*)&client_addr, &sin_size);
                if (candidate_fd == -1) {
                    // cerr << "server accept failed or no connection attempt" << endl;
                    this_thread::sleep_for(std::chrono::microseconds(100));
                    continue;
                }

                char client_info_buffer[INET6_ADDRSTRLEN];
                inet_ntop(client_addr.ss_family, get_in_addr((struct sockaddr*)&client_addr), client_info_buffer, sizeof client_info_buffer);
                cout << "server: got new connection from " << client_info_buffer << endl;

                if (!allow_new_connection(client_addr, sin_size)) {
                    cout << "Only one connection allowed at a time; closing new connection." << endl;
                    cross_close(candidate_fd);
                    continue;
                }

                Connection * remote_connection = new Connection;
                RemoteConnectionResult result = init_remote_connection(remote_connection, candidate_fd);
                switch (result) {
                case FAIL:
                    delete remote_connection;
                    return;
                case CONTINUE:
                    delete remote_connection;
                    continue;
                case OK:
                    got_new_connection(client_addr, sin_size);
                    break;
                }   
            }
        }
        else {
            sendAndReceive(&local_connection);
            set_connect_error(ConnectError::SUCCESS);
        }
    }

    cout << "exited connect thread" << endl;
}

ConnectError Comm::send_one(Connection * connection, MessageData * message_data) {
    pollfd ufds[1];
    ufds[0].fd = connection->sock_fd;
    ufds[0].events = POLLOUT;
    
    int poll_result = cross_poll(ufds, 1, 500);
    if (poll_result == -1) {
        set_connect_error(SEND_POLL_ERROR);
        connection->keep_going_flag = false;
        cerr << "send poll error" << endl;
        return SEND_POLL_ERROR;
    }

    if (poll_result == 0) {
        // possibly means socket is already busy sending?
        return SEND_TIMEOUT;
    }
   
    const string header = message_data->serialize_header();
    auto header_size = header.size();
    auto image_size = message_data->image_data.size();
    long sent = 0;
    switch (role) {
        case Role::SERVER:
        case Role::CLIENT:
            auto begin = SteadyClock::now();
            sent = ::send(connection->sock_fd, header.data(), header_size, 0);
            if (message_data->image_data.size() > 0) {
                sent += ::send(connection->sock_fd, message_data->image_data.data(), image_size, 0);
            }
            Seconds seconds = (SteadyClock::now() - begin);
            cout << "sent h:" << header_size << " ty:" << static_cast<int>(header[0]) << " i:" << image_size << " t:" << seconds.count() << "s" << endl;
            break;
    }
    if (--message_data->use_count <= 0) {
        // cout << "comm deleting message data " << message_data->message_type << endl;
        if (message_data->auto_delete) {
            delete message_data;
        }
    }

    if (sent != header_size + image_size) {
        set_connect_error(SEND_COUNT_FAILURE);
        connection->keep_going_flag = false;
        cerr << "send count failure sent:" << sent << " hs:" << header_size << " is:" << image_size << endl;
        return SEND_COUNT_FAILURE;
    }
    
    return ConnectError::SUCCESS;
}

void Comm::execute_send(Connection * remote_connection) {
    while (true) {
        while (MessageData * message_data = remote_connection->next_send()) {
            ConnectError result = send_one(remote_connection, message_data);
            if (result == ConnectError::SUCCESS) {
                continue;
            }
            
            long counter = 0;
            while (result == SEND_TIMEOUT && counter < 100) {
                result = send_one(remote_connection, message_data);
                counter += 1;
                this_thread::sleep_for(std::chrono::microseconds(10));
            }
            
            if (result == ConnectError::SUCCESS) {
                continue;
            }
            
            break;
        }

        if (!remote_connection->keep_going_flag) {
            break;
        }

        this_thread::sleep_for(std::chrono::microseconds(10));
    }

    cout << "exited send thread" << endl;
}

void Comm::execute_receive(Connection * remote_connection) {
    pollfd ufds[1];
    ufds[0].fd = remote_connection->sock_fd;
    ufds[0].events = POLLIN;
    char buffer[512];
    long counter = 0;
    
    SD sd;

    while (true) {
        int poll_result = cross_poll(ufds, 1, 500);
        if (poll_result == -1) {
            set_connect_error(RECEIVE_POLL_ERROR);
            remote_connection->keep_going_flag = false;
            cerr << "receive poll error" << endl;
            break;
        }

        if (poll_result == 0) {
            counter += 1;
            // cout << "receive timeout " << counter << endl;
        }
        else {
            if (ufds[0].events & POLLIN) {
                counter = 0;
                long received_count = 0;
                switch (this->role) {
                    case Role::SERVER:
                    case Role::CLIENT:
                        received_count = recv(remote_connection->sock_fd, buffer, sizeof(buffer), 0);
                        break;
                }
                if (received_count <= 0) {
                    cerr << "remote disconnected while looking for incoming" << endl;
                    set_connect_error(SERVER_DISCONNECTED);
                    if (is_server()) {
                        cross_close(remote_connection->sock_fd);
                        lock_guard<mutex> guard(this->remote_connections_mutex);
                        this->remote_connections.remove(remote_connection);
                        this->deleted_remote_connections.emplace_back(remote_connection);
                    }
  
                    remote_connection->keep_going_flag = false;
                    break;
                }

                remote_connection->received_so_far.append(buffer, received_count);
                if (message_state == MessageState::WAITING) {
                    message_state = MessageState::WAITING_FOR_HEADER;
                    receive_begin = SteadyClock::now();
                }
                
                if (remote_connection->received_so_far.size() >= MessageData::header_size) {
                    switch (message_state) {
                        case MessageState::WAITING:
                        case MessageState::WAITING_FOR_HEADER:
                            message_state = MessageState::STARTED;
                            // cout << "started" << endl;
                            break;
                        case MessageState::STARTED:
                            message_state = MessageState::ONGOING;
                            // cout << "ongoing" << endl;
                            break;
                        case MessageState::ONGOING:
                            break;
                    }
                }

                // cout << "so far:" << received_so_far << endl;
                while (MessageData * message_data = MessageData::deserialize(remote_connection->received_so_far, message_state)) {

                    Seconds seconds = SteadyClock::now() - this->receive_begin;
                    cout << "receive i:" << message_data->image_data.size() << " t:" << seconds.count() << "s" << endl;
                    
                    if (message_data->message_type == MessageData::MessageType::DISPLAY_NOW) {
                        sd.increment(SteadyClock::now());
                        if (sd.count % 30 == 0) {
                            std::ofstream out("server_dn_counter.txt");
                            sd.dump(out, "DISPLAY_NOW");
                        }
                    }
                    message_state = MessageState::WAITING;
                    {
                        // lock within tight scope
                        lock_guard<mutex> guard(this->received_values_mutex);
                        received_values.emplace_back(message_data);
                    }
                    
                    if (waiter) {
                        waiter->notify();
                    }
                }
            }
        }

        if (!remote_connection->keep_going_flag) {
            break;
        }
    }

    cout << "exited receive thread" << endl;
}

bool Comm::connect(Role pending_role, const string & ip_address, const string & port) {
    if (connect_thread != nullptr) {
        return false;
    }

    this->ip_address = (pending_role == Role::CLIENT ? ip_address : "localhost");
    this->ip_port = port;

    cout << "attempting to connect to " << this->ip_address << ":" << port << " as " << (pending_role == Role::CLIENT ? "client" : "server") << endl;

    // this->role isn't set until the execute_connect thread runs
    connect_thread = new thread(&Comm::execute_connect, this, pending_role, this->ip_address, this->ip_port);
    return true;
}

ConnectError Comm::connect_result() {
    lock_guard<mutex> guard(this->connect_result_mutex);
    return connect_error;
}

void Comm::disconnect() {
    if (connect_thread == nullptr) {
        return;
    }

    cout << "disconnecting" << endl;

    this->local_connection.keep_going_flag = false;

    connect_thread->join();
    delete connect_thread;
    connect_thread = nullptr;
    local_connection.stop();

    if (is_server()) {
        close_all();
    }

    cross_close(this->local_connection.sock_fd);
}

void Comm::close_one(Connection* remote_connection) {
    remote_connection->stop();
    if (remote_connection->sock_fd >= 0) {
#ifdef USING_SSL
        remote_connection->close_ssl();
#endif
        cross_close(remote_connection->sock_fd);
        delete remote_connection;
    }
}

void Comm::close_all() {
    lock_guard<mutex> guard(this->remote_connections_mutex);
    for (Connection * remote_connection: this->remote_connections) {
        close_one(remote_connection);
    }
    for (Connection* remote_connection : this->deleted_remote_connections) {
        close_one(remote_connection);
    }

    remote_connections.clear();
    deleted_remote_connections.clear();
}

ConnectError Comm::send(MessageData * message_data, BlockType block) {
    bool result = false;
    if (is_server()) {
        lock_guard<mutex> guard(this->remote_connections_mutex);
        message_data->use_count = static_cast<int>(this->remote_connections.size());
        for (Connection* remote_connection : this->remote_connections) {
            if (block == BLOCKING) {
                auto result = this->send_one(remote_connection, message_data);
                if (result != ConnectError::SUCCESS) {
                    return result;
                }
            }
            else {
                remote_connection->send(message_data);
            }
        }
    }
    else {
        message_data->use_count = 1;
        if (block == BLOCKING) {
            this->send_one(&this->local_connection, message_data);
        }
        else {
            this->local_connection.send(message_data);
        }
    }
    
    return ConnectError::SUCCESS;
}

MessageData * Comm::next_received() {
    lock_guard<mutex> guard(this->received_values_mutex);
    if (received_values.empty()) {
        return nullptr;
    }

    MessageData * message_data = this->received_values.front();
    this->received_values.pop_front();

    return message_data;
}

void Comm::send_display_now(const string & image_name) {
    this->send(new MessageData(MessageData::MessageType::DISPLAY_NOW, image_name));
}

void Comm::send_image(const string & image_name, const string & image_data) {
    this->send(new MessageData(MessageData::MessageType::IMAGE, image_name, image_data));
}

void Comm::send_start_timer() {
    this->send(new MessageData(MessageData::MessageType::START_TIMER));
}

void Comm::send_ack(const string &image_name) {
    this->send(new MessageData(MessageData::MessageType::ACK, image_name));
}

void Comm::set_waiter(Waiter *waiter) {
    this->waiter = waiter;
}

double SD::increment(const SteadyClock::time_point & current, const SteadyClock::time_point & previous) {
    Seconds one_unit = current - previous;
    return this->increment(one_unit);
}

double SD::increment(const SteadyClock::time_point & current) {
    Seconds one_frame = current - this->last;
    auto result = this->increment(one_frame);
    this->last = current;
    return result;
}
    
double SD::increment(const Seconds & seconds) {
    double seconds_d = seconds.count();
    this->count += 1;
    // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Welford's_online_algorithm
    // there is some floating point error, but since the subtractions compare
    // same order-of-magnitude values, it makes a decent estimate
    if (this->count == SD::warm_up) {
        // initial value (let some initial frames go by for stabilization)
        this->mean = seconds_d;
        this->sum_squares = 0;
    }
    else if (this->count > SD::warm_up) {
        auto new_count = this->count - SD::warm_up + 1;
        auto d1 = seconds_d - this->mean;
        auto new_mean = this->mean + (d1 / new_count);
        auto d2 = seconds_d - new_mean;
        auto new_sum_squares = this->sum_squares + (d1 * d2);
        {
            lock_guard<mutex> guard(this->sd_mutex);
            // push the sd
            this->sd_q.push_back(sqrt(new_sum_squares / new_count));
        }
        this->mean = new_mean;
        this->sum_squares = new_sum_squares;
    }
    return seconds_d;
}

void SD::dump(ofstream & out, const string & label) {
    lock_guard<mutex> guard(this->sd_mutex);
    out << label << ": " << this->count << " - " << SD::warm_up << endl;
    out << "mean: " << this->mean << endl;
    out << "sd:   ";
    while (!this->sd_q.empty()) {
        auto sd = this->sd_q.front();
        this->sd_q.pop_front();
        out << sd << " ";
    }
    out << endl;
}

void Display::queue_image_for_display(MessageData * message_data) {
    lock_guard<mutex> guard(this->queues_mutex);
    pending_images[message_data->image_name] = message_data;
    pending_q_count += 1;
    // cout << "+pending_q " << message_data->image_name << " qlen:" << pending_images.size() << endl;
}

void Display::dump(ofstream & out) {
    lock_guard<mutex> guard(this->queues_mutex);
    out << "displayed: " << this->display_count << endl
    << "all_display_q: " << this->display_q_count << endl
    << "all_pending_q: " << this->pending_q_count << endl
    << "still_pending: " << this->pending_images.size() << endl
    << "still_display: " << this->images_to_display.size() << endl
    << "not_found: " << this->name_not_found_count << endl;
}

void Display::image_should_be_displayed(string image_name) {
    lock_guard<mutex> guard(this->queues_mutex);
    images_to_display.push_back(image_name);
    // cout << "+to_display_q " << image_name << " qlen:" << images_to_display.size() << endl;
    display_q_count += 1;
}

void Display::set_display_function(DisplayFunction display_function) {
    this->display_function = display_function;
}

void Display::execute_display() {
    this->fwrite_sd.last = SteadyClock::now();
    while (keep_going) {
        MessageData * to_display = nullptr;
        {
            lock_guard<mutex> guard(this->queues_mutex);
            if (!images_to_display.empty()) {
                string image_name = this->images_to_display.front();
                this->images_to_display.pop_front();
                try {
                    pending_images.at(image_name);
                    to_display = pending_images[image_name];
                    pending_images.erase(image_name);
                    // cout << "+display " << image_name << " pqlen:" << pending_images.size() << " tdqlen:" << images_to_display.size() << endl;
                }
                catch (const out_of_range &e) {
                    cout << image_name << " not in pending queue " << e.what() << endl;
                    name_not_found_count += 1;
                }
            }
        }
        if (to_display) {
            display_count += 1;
            auto begin = SteadyClock::now();
            if (this->display_function) {
                this->display_function(to_display->image_data);
            }
            auto current = SteadyClock::now();
            auto one_frame = this->fwrite_sd.increment(current);
            Seconds elapsed = current - begin;
            cout << "+fwrite:" << to_display->image_name << " elapsed:" << elapsed.count() << "s 1f:" << one_frame << endl;
            delete to_display;
        }
        
        this_thread::sleep_for(std::chrono::microseconds(1));
    }
}
