#include <iostream>
#include <thread>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <condition_variable>
#include <mutex>
#include <cstring>
#include <deque>
#include <fstream>
#include <unordered_map>
#include <csignal>

#include "comms.h"

inline bool ends_with(std::string const & value, std::string const & ending)
{
    if (ending.size() > value.size()) return false;
    return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

void pipe_closed_handler( int signum ) {
    cout << endl << "Interrupt signal (" << signum << ") received." << endl << endl;

    if (signum == SIGPIPE) {
        // stop the server?
    }
}

struct Params {
    long bits_per_pixel = 8;
    long video_width = 1024;
    long video_height = 768;
    double ffplay_frame_rate = 60;
    string ffplay_pixel_format = "gray";
    bool ffplay_flag = false;
    string ffplay_args;
    
    int get_params(int argc, char* argv[]) {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-ff") == 0) {
                ffplay_flag = true;
                // all params following -ff will be forwarded to ffplay
                for (int j = i + 1; j < argc; j++) {
                    // note: adding the space before, as we have a space after when the pipe is opened with a constructed command
                    ffplay_args += " ";
                    ffplay_args += argv[j];
                }
                break;
            }
        }
        
        for (int i = 1; i < argc - 1; i++) {
            if (strcmp(argv[i],"-w") == 0) {
                video_width = strtol(argv[i + 1], nullptr, 10);
                if (video_width == 0 || errno != 0) {
                    cout << "unable to parse video_width" << endl;
                    return -1;
                }
            }
            else if (strcmp(argv[i],"-h") == 0) {
                video_height = strtol(argv[i + 1], nullptr, 10);
                if (video_height == 0 || errno != 0) {
                    cout << "unable to parse video_height" << endl;
                    return -1;
                }
            }
            else if (strcmp(argv[i],"-f") == 0) {
                ffplay_frame_rate = strtol(argv[i + 1], nullptr, 10);
                if (ffplay_frame_rate == 0 || errno != 0) {
                    cout << "unable to parse ffplay_frame_rate" << endl;
                    return -1;
                }
            }
            else if (strcmp(argv[i],"-bpp") == 0) {
                bits_per_pixel = strtol(argv[i + 1], nullptr, 10);
                if (bits_per_pixel == 0 || errno != 0) {
                    cout << "unable to parse bits_per_pixel" << endl;
                    return -1;
                }
            }
            else if (strcmp(argv[i],"-pf") == 0) {
                ffplay_pixel_format = argv[i + 1];
            }
        }
        
        return 0;
    }
};

void usage(const Params & params) {
    cout << "Sample MRR_Pi server code handling display and image messages." << endl;
    cout << "Each server instance is meant to be paired with a single instance of MRR_Pi_client." << endl;
    cout << endl;
    cout << "usage: MRR_Pi_server" << endl;
    cout << "  [-p port number, range 1024 to 49151, default = " << Comm::default_port << " ]" << endl;
    cout << "  [-w video_width, default = " << params.video_width << " ]" << endl;
    cout << "  [-h video_height, default = " << params.video_height << " ]" << endl;
    cout << "  [-bpp bits_per_pixel, default = " << params.bits_per_pixel  << " ]" << endl;
    cout << "  [-pf fplay_pixel_format, default = " << params.ffplay_pixel_format << " ]" << endl;
    cout << "  [-f ffplay_frame_rate, used if -ff is specified, default = " << params.ffplay_frame_rate << " ]" << endl;
    cout << "    ffplay throttles frame writes to the pipe to control its frame rate," << endl;
    cout << "    so ffplay's frame rate should be higher than the actual desired frame rate" << endl;
    cout << "    (as set in the controller app)." << endl;
    cout << "    A good rule of thumb is 2x, so if desired frame rate is 30, -f could be 60." << endl;
    cout << "  [-ff ..., output frames to ffplay, followed by optional ffplay arguments]" << endl;
    cout << endl;
    cout << "The -ff option displays received images in ffplay via pipe (default is no display)" << endl;
    cout << "There is currently no checking whether ffplay is installed." << endl;
    cout << "The -ff argument must follow all previous arguments (-p, -w, -h, etc) in the command line (if any) ." << endl;
    cout << "All arguments following -ff will be passed directly to ffplay." << endl;
    cout << endl;
    
    cout << "sample command line (runs server on the default port): ./MRR_Pi_server" << endl;
    cout << "sample command line (specifies port): ./MRR_Pi_server -p 5577" << endl;
    cout << "sample command line (ffplay -fs == fullscreen): ./MRR_Pi_server -p 5577 -ff -fs -loglevel quiet" << endl;
    cout << endl;
}

// these are global to make them more easily available to the display function using ffplay
FILE * pipe_out = nullptr;
long video_buffer_size = 0;

int main(int argc, char* argv[]) {

    Params params;
    usage(params);
    
    int result = params.get_params(argc, argv);
    if (result != 0) {
        return result;
    }
    
    video_buffer_size = params.video_width * params.video_height * params.bits_per_pixel / 8;
    auto display = new Display();
    
    Waiter waiter;
    Comm * comm = Comm::start_server(&waiter, argc, argv);
    if (comm == nullptr) {
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
    
    // cache image data for later checking against incoming image data
    std::unordered_map<std::string, string> file_strings;
    for (auto & filename : files) {
        file_strings[filename] = load_image(filename);
    }
    // end debugging
    
    if (params.ffplay_flag) {
        // the '-i -' designates video will be incoming from a pipe
        string cmd = string("ffplay -f rawvideo -vcodec rawvideo -pixel_format ")
                            + params.ffplay_pixel_format
                            + " "
                            + "-framerate "
                            + std::to_string(params.ffplay_frame_rate)
                            + " "
                            + "-video_size "
                            + std::to_string(params.video_width)
                            + "x"
                            + std::to_string(params.video_height)
                            + " "
                            + params.ffplay_args
                            + " -i -";
        

        // if ffplay window is closed, it triggers a SIGPIPE signal
        signal(SIGPIPE, pipe_closed_handler);
        pipe_out = popen(cmd.c_str(), "w");
        
        display->set_display_function([] (const string & image_data) -> void {
            // note: display function should do the minimum possible work and get out
            // could use OpenCV here instead of writing to the ffplay pipe
            fwrite(image_data.c_str(), 1, video_buffer_size, pipe_out);
        });

        cout << cmd << endl;
        cout << endl;
        display->display_thread = new thread([display]() { display->execute_display(); });
    }
    
    long image_count = 0;
    long matched_count = 0;
    long mismatched_count = 0;
    auto begin = SteadyClock::now();
    
    while (true) {
        waiter.wait();
        deque<MessageData *> received_messages;
        deque<MessageData *> to_delete;
        while (auto message_data = comm->next_received()) {
            received_messages.push_back(message_data);
        }
        
        if (received_messages.empty()) {
            this_thread::sleep_for(std::chrono::microseconds(10));
            continue;
        }
        
        for (auto message_data : received_messages) {
            bool do_delete = true;
            if (message_data->message_type == MessageData::MessageType::DISPLAY_NOW) {
                // for debugging
                auto current = SteadyClock::now();
                Seconds elapsed = current - begin;
                auto one_frame = display->display_sd.increment(current);
                cout << "display now " << message_data->image_name << " t:" << elapsed.count() << "s 1f:" << one_frame << endl;
                // end debugging
                
                if (params.ffplay_flag) {
                    // signal that queued image can be displayed
                    display->image_should_be_displayed(message_data->image_name);
                }
                
                // for debugging
                std::ofstream out("server_counter.txt");
                out << "t:" << elapsed.count() << "s" << endl;
                out << "images_rec'd: " << image_count << endl;
                out << "match: " << matched_count << endl;
                out << "mismatch: " << mismatched_count << endl;
                display->dump(out);
                display->display_sd.dump(out, "display");
                display->fwrite_sd.dump(out, "fwrite");
                out.close();
                // end debugging
            }
            else if (message_data->message_type == MessageData::MessageType::IMAGE) {
                cout << "got image '" << message_data->image_name << "' sz:" << message_data->image_data.size() << endl;
                
                // tell the sender the image has been received
                // this helps keep the sender from sending images too quickly
                comm->send_ack(message_data->image_name);
                
                if (params.ffplay_flag && message_data->image_data.size() >= display->video_buffer_size) {
                    // queue image for later display
                    display->queue_image_for_display(message_data);
                    do_delete = false;
                }
                
                image_count += 1;
                
                // for debugging
                for (auto filename : files) {
                    if (message_data->image_name.find(filename) != string::npos) {
                        if (message_data->image_data == file_strings[filename]) {
                            matched_count += 1;
                        }
                        else {
                            mismatched_count += 1;
                        }
                        break;
                    }
                }
                // end debugging
            }
            else if (message_data->message_type == MessageData::MessageType::START_TIMER) {
                begin = SteadyClock::now();
            }
            if (do_delete) {
                to_delete.push_back(message_data);
            }
        }
        
        // delete unwanted messages
        for (auto message_data : to_delete) {
            // cout << "deleting ty:" << message_data->message_type << " " << message_data->image_name << endl;
            delete message_data;
        }
        // cleared by scope but wtf
        to_delete.clear();
        
        // cleared by scope but wtf
        received_messages.clear();
    }

    return 0;
}
