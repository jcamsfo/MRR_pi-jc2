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

#include "comms.h"


void usage() {
    cout << "usage: MRR_Pi_client [-r repeat_count]  [-f fps] [-p port_number] [-i ip_address] [-p port_number] [-i ip_address] ..." << endl;
    cout << endl;
    cout << "Sample MRR_Pi client code which sends images to one or more MRR_Pi servers." << endl;
    cout << "Each server is described by both a port_number and an ip_address," << endl;
    cout << "so the count of port_numbers must mach the count of ip_addresses," << endl;
    cout << "which are paired by their order in the command line." << endl;
    cout << "If both MRR_Pi_Client and MRR_Pi_server are on the same machine, use 127.0.0.1 as the ip address." << endl;
    cout << "Repeat_count defaults to 100, it is the total number of image files to send to the server, repeatedly looping through the 5 images in the 'raw' folder." << endl;
    cout << "Default fps is 30" << endl;
    cout << endl;
    
    cout << "sample command line (server is running on default port on localhost): ./MRR_Pi_client" << endl;
    cout << "sample command line (specify port and ip_address): ./MRR_Pi_client -i 127.0.0.1 -p 5569" << endl;
    cout << "sample command line (two servers specified): ./MRR_Pi_client -i 127.0.0.1 -p 5569 -i 127.0.0.1 -p 5570" << endl;
    cout << endl;
}

class CommPlus : public Comm {
    
public:
    long skip_count = 0;
    long unack_count = 0;
    SD display_sd;
    map<string, long> waiting_for_ack;  // preserves order of entry
};

// used to create the subclass CommPlus instead of the default Comm class
Comm * comm_factory() {
    return new CommPlus();
}

struct Params {
    long long repeat_count = 100;
    float fps = 30;
    
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

int main(int argc, char* argv[]) {
    usage();
    
    Params params;
    int result = params.get_params(argc, argv);
    if (result != 0) {
        return result;
    }
    
    const double timer_fudge = (1 / params.fps) / 100;
    
    Waiter waiter;
    list<Comm *> comms = Comm::start_clients(&waiter, argc, argv, comm_factory);
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
    
    long late_count = 0;
    auto begin = SteadyClock::now();
    long unack_count = 0;
    for (unsigned long long i = 0; i < params.repeat_count; i++) {
        // gather and process image here
        
        // test using sample images
        // at each step send one of the file images, using modulo
        auto raw_filename = files[i % files_len];
        // don't really have to load the file every time, but it simulates some work being done
        string image_data = load_image(raw_filename);
        auto send_name = raw_filename + "__" + to_string(i);
        // end test
        
        cout << "sending:" << send_name << " len:" << image_data.size() << " " << i << endl;
        for (auto comm : comms) {
            CommPlus * comm_plus = static_cast<CommPlus *>(comm);
            if (true || comm_plus->waiting_for_ack.size() <= 1) {
                comm_plus->send_image(send_name, image_data);
                comm_plus->waiting_for_ack[send_name] = i;
            }
            else {
                comm_plus->skip_count += 1;
                for (auto & iter : comm_plus->waiting_for_ack) {
                    cout << "skipping after:" << iter.first << " " << iter.second << endl;
                }
            }
        }
        
        // 'sleep' until it's time to send DISPLAY_NOW messages
        double goal = (i + 1) / params.fps;
        Seconds elapsed = SteadyClock::now() - begin;
        long loop_count = 0;
        while (elapsed.count() < goal - timer_fudge) {
            loop_count += 1;
            auto wait_result = waiter.wait_for(Seconds(goal - elapsed.count()));
            if (wait_result == cv_status::timeout) {
                for (auto comm : comms) {
                    CommPlus * comm_plus = static_cast<CommPlus *>(comm);
                    while (auto message_data = comm_plus->next_received()) {
                        if (message_data->message_type == MessageData::MessageType::ACK) {
                            try {
                                comm_plus->waiting_for_ack.erase(message_data->image_name);
                                cout << "ack:" << message_data->image_name << endl;
                            }
                            catch (const out_of_range &e) {
                                cerr << message_data->image_name << " not in waiting_for_ack " << e.what() << endl;
                            }
                            
                        }
                        delete message_data;
                    }
                }
            }
            elapsed = SteadyClock::now() - begin;
        }
        if (loop_count == 0) {
            // how often do we fall behind?
            late_count += 1;
        }

        cout << "elapsed:" << elapsed.count() << " goal:" << goal << " g-e:" << goal - elapsed.count() << " late:" << late_count << endl;

        // send DISPLAY_NOW to each display
        for (auto comm : comms) {
            CommPlus * comm_plus = static_cast<CommPlus *>(comm);
            // optionally use waiting_for_ack to throttle sends
            if (true || comm_plus->waiting_for_ack.size() <= 2) {
                comm_plus->send_display_now(send_name);
                comm_plus->display_sd.increment(SteadyClock::now());
            }
            else {
                while (comm_plus->waiting_for_ack.size() > 1) {
                    comm_plus->unack_count += 1;
                    comm_plus->waiting_for_ack.erase(prev(comm_plus->waiting_for_ack.end()));
                }
            }
        }
        
        // for debugging
        std::ofstream out("client_counter.txt");
        out << "t: " << elapsed.count() << "s" << endl;
        out << "frame: " << i + 1 << endl;
        out << "skipped: ";
        for (auto comm : comms) {
            CommPlus * comm_plus = static_cast<CommPlus *>(comm);
            out << comm_plus->skip_count << " ";
        }
        out << endl;
        out << "unack: ";
        for (auto comm : comms) {
            CommPlus * comm_plus = static_cast<CommPlus *>(comm);
            out << comm_plus->unack_count << " ";
        }
        out << endl;
        out << "late: " << late_count << endl;
        for (auto comm : comms) {
            CommPlus * comm_plus = static_cast<CommPlus *>(comm);
            comm_plus->display_sd.dump(out, comm_plus->ip());
        }
        out.close();
        // end debugging
    }
    
    // give the server time to process the last sends before the connection is dropped
    this_thread::sleep_for(std::chrono::seconds(1));
    
    for (auto comm : comms) {
        CommPlus * comm_plus = static_cast<CommPlus *>(comm);
        
        for (auto & iter : comm_plus->waiting_for_ack) {
            cout << "missing ack for:" << iter.first << " " << iter.second << endl;
        }
    }

    return 0;
}
