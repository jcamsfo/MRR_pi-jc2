#include <iostream>
#include <thread>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <fstream>
#include <chrono>
#include <list>
#include <cstring>
#include <map>
#include <stdlib.h>

// #include <pthread.h>

#include "comms.h"




void usage() {
    cout << "usage: MRR_Pi_client_2 [-r repeat_count]  [-f fps] [-p port_number] [-i ip_address] [-p port_number] [-i ip_address] ..." << endl;
    cout << endl;
    cout << "Sample MRR_Pi client code which sends images to one or more MRR_Pi servers." << endl;
    cout << "Each server is described by both a port_number and an ip_address," << endl;
    cout << "so the count of port_numbers must mach the count of ip_addresses," << endl;
    cout << "which are paired by their order in the command line." << endl;
    cout << "If both MRR_Pi_Client_2 and MRR_Pi_server_2 are on the same machine, use 127.0.0.1 as the ip address." << endl;
    cout << "Repeat_count defaults to 0 (loop forever), it is the total number of image files to send to each server, repeatedly picking from the 5 images in the 'raw' folder." << endl;
    cout << "Default fps is 30" << endl;
    cout << endl;
    
    cout << "sample command line (server is running on default port on localhost): ./MRR_Pi_client_2" << endl;
    cout << "sample command line (specify port and ip_address): ./MRR_Pi_client_2 -i 127.0.0.1 -p 5569" << endl;
    cout << "sample command line (two servers specified): ./MRR_Pi_client_2 -i 127.0.0.1 -p 5569 -i 127.0.0.1 -p 5570" << endl;
    cout << endl;
}

struct Params {
    long repeat_count = 30 * 60 * 10;
    float fps = .5;   // was30  1.1 seconds per image
    
    int get_params(int argc, char* argv[]) {
        for (int i = 1; i < argc - 1; i++) {
            if (strcmp(argv[i],"-r") == 0) {
                repeat_count = strtoll(argv[i + 1], nullptr, 10);
                if (repeat_count == 0 || errno != 0) {
                    cout << "unable to parse repeat_count" << endl;
                    return -1;
                }
            }
            if (strcmp(argv[i],"-f") == 0) {
                fps = strtof(argv[i + 1], nullptr);
                if (fps == 0 || errno != 0) {
                    cout << "unable to parse fps" << endl;
                    return -1;
                }
            }
        }
        
        return 0;
    }
};

class CommPlus : public Comm {
public:
    SD blocking_sd;
};

// used to create the subclass CommPlus instead of the default Comm class
Comm * comm_factory() {
    return new CommPlus();
}


int main(int argc, char* argv[]) {
    usage();
    
    auto blocking_send = Comm::NON_BLOCKING;
    
    Params params;
    int result = params.get_params(argc, argv);
    if (result != 0) {
        return result;
    }
    
    list<Comm *> comms = Comm::start_clients(nullptr, argc, argv, comm_factory);
    if (comms.empty()) {
        return -1;
    }
    
    // for debugging
    string files[] = {
        "../raw/24-06-03-04-30-10.raw",
        "../raw/24-06-03-04-31-09.raw",
        "../raw/24-06-03-04-35-02.raw",
        "../raw/24-06-03-04-32-06.raw",
        "../raw/24-06-03-04-37-06.raw"
    };
    
    auto files_len = sizeof(files)/sizeof(files[0]);
    // end debugging
        
    for (auto comm : comms) {
        comm->send_start_timer();
    }
    
    SD blocking_sd;
    SD loop_sd;
    long late_count = 0;
    auto begin = SteadyClock::now();
    long unack_count = 0;
    for (long loop_count = 0; loop_count < params.repeat_count; loop_count++) {
        // gather and process image here
        list<string> images_to_send;
        list<string> names_to_send;
        for (auto & comm : comms) {
            // don't really have to load the file every time, but it simulates work being done
            auto raw_filename = files[rand() % files_len];
            string image_data = load_image(raw_filename);
            images_to_send.push_back(image_data);
            auto send_name = raw_filename + "__" + to_string(loop_count);
            names_to_send.push_back(send_name);
        }
        
        // 'sleep' until it's time to send images
        // this assumes loop_count doesn't overflow
        
        double goal = (loop_count + 1) / params.fps;
        Seconds elapsed = SteadyClock::now() - begin;
        if (elapsed.count() < goal) {
            this_thread::sleep_for(std::chrono::duration<double>(goal - elapsed.count()));
        }
        
        auto before_send = SteadyClock::now();
        
        // now send
        for (auto & comm: comms) {
            string image_data = images_to_send.front();
            images_to_send.pop_front();
            string send_name = names_to_send.front();
            names_to_send.pop_front();
            if (blocking_send == Comm::NON_BLOCKING) {
                comm->send_image(send_name, image_data);
            }
            else {
                // note that message_data is deleted automatically by send() unless you set message_data->auto_delete = false;
                MessageData * message_data = new MessageData(MessageData::MessageType::IMAGE, send_name, image_data);
                auto before = SteadyClock::now();
                auto result = comm->send(message_data, Comm::BLOCKING);
                CommPlus * comm_plus = static_cast<CommPlus *>(comm);
                comm_plus->blocking_sd.increment(SteadyClock::now(), before);
                if (result != ConnectError::SUCCESS) {
                    cout << "send failure " << result << endl;
                    return -1;
                }
            }
        }
        
        Seconds send_elapsed = SteadyClock::now() - before_send;
        cout << "elapsed:" << elapsed.count() << " goal:" << goal << " g-e:" << goal - elapsed.count() << " send:" << send_elapsed.count() << endl;
        
        loop_sd.increment(SteadyClock::now());
        
        // for debugging
        std::ofstream out("client_counter_2.txt");
        out << "t: " << elapsed.count() << "s" << endl;
        out << "frame: " << loop_count + 1 << endl;
        if (blocking_send == Comm::BLOCKING) {
            for (auto & comm : comms) {
                CommPlus * comm_plus = static_cast<CommPlus *>(comm);
                comm_plus->blocking_sd.dump(out, "blocking send " + comm_plus->ip() + " " + comm_plus->port());
            }
            
        }
        loop_sd.dump(out, "loop");
        out.close();
        // end debugging
    }
    

    
    // give the server time to process the last sends before the connection is dropped
    this_thread::sleep_for(std::chrono::seconds(1));


    
    return 0;
}
