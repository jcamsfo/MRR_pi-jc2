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
#include <limits>
#include <opencv2/opencv.hpp>

// #include <pthread.h>

#include "comms.h"

inline bool ends_with(std::string const &value, std::string const &ending)
{
    if (ending.size() > value.size())
        return false;
    return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

void usage()
{
    cout << "Sample MRR_Pi server code handling display and image messages." << endl;
    cout << "Each server instance is meant to be paired with a single instance of MRR_Pi_client." << endl;
    cout << endl;
    cout << "usage: MRR_Pi_server" << endl;
    cout << "  [-p port number, range 1024 to 49151, default = " << Comm::default_port << " ]" << endl;
    cout << endl;

    cout << "sample command line (runs server on the default port): ./MRR_Pi_server" << endl;
    cout << "sample command line (specifies port): ./MRR_Pi_server -p 5577" << endl;
    cout << endl;
}

int main(int argc, char *argv[])
{



    // create a gradient for test image using a pointer
    int width = 1024;
    int height = 768;
    int size = width * height;
    uchar *dataX = new uchar[size];
    for (int i = 0; i < size; ++i)
    {
        dataX[i] = i / 3072; // Example: gradient effect
    }

    // use memcopy to convert Jonathan's container to an opencv Mat   // had ame offset reults
    // cv::Mat image(height, width, CV_8UC1);      // Create an empty cv::Mat with the desired dimensions
    // memcpy(image.data, dataX, size * sizeof(uchar));      // Copy the data from the 1D array to the cv::Mat

    // creat 2 empty Mats
    cv::Mat image, image_test;


    // for timing various things
    auto start_check = std::chrono::high_resolution_clock::now();
    auto end_check = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_check - start_check;





    usage();

    double fps = 30;

    Comm *comm = Comm::start_server(nullptr, argc, argv);
    if (comm == nullptr)
    {
        return -1;
    }

    // for debugging
    string files[] = {
        "../raw/24-06-03-04-30-10.raw",
        "../raw/24-06-03-04-31-09.raw",
        "../raw/24-06-03-04-35-02.raw",
        "../raw/24-06-03-04-32-06.raw",
        "../raw/24-06-03-04-37-06.raw"};

    // cache image data for later checking against incoming image data
    std::unordered_map<std::string, string> file_strings;
    for (auto &filename : files)
    {
        file_strings[filename] = load_image(filename);
    }
    // end debugging

    long image_count = 0;
    long matched_count = 0;
    long mismatched_count = 0;
    auto begin = SteadyClock::now();

    long max_loop = std::numeric_limits<long>::max();

    SD loop_sd;
    deque<MessageData *> cached_messages;

    for (long loop_count = 0; loop_count < max_loop; loop_count++)
    {
        deque<MessageData *> to_delete;
        deque<MessageData *> received_messages;
        while (auto message_data = comm->next_received())
        {
            received_messages.push_back(message_data);
        }

        // loop here isn't strictly necessary, since images will probably arrive one at a time
        for (auto message_data : received_messages)
        {
            bool do_delete = true; // delete messages that don't contain images
            if (message_data->message_type == MessageData::MessageType::IMAGE)
            {
                do_delete = false;
                cached_messages.push_back(message_data);

                // for debugging
                cout << "got image '" << message_data->image_name << "' sz:" << message_data->image_data.size() << endl;

                image_count += 1;

                for (auto filename : files)
                {
                    if (message_data->image_name.find(filename) != string::npos)
                    {
                        if (message_data->image_data == file_strings[filename])
                        {
                            matched_count += 1;
                        }
                        else
                        {
                            mismatched_count += 1;
                        }
                        break;
                    }
                }
                // end debugging
            }

            if (do_delete)
            {
                to_delete.push_back(message_data);
            }
        }

        while (cached_messages.size() > 2)
        {
            to_delete.push_back(cached_messages.front());
            cached_messages.pop_front();
        }

        // delete unwanted messages
        for (auto message_data : to_delete)
        {
            cout << "deleting ty:" << message_data->message_type << " " << message_data->image_name << endl;
            delete message_data;
        }

        // do some processing of cached messages here


        // try 2 ways of converting to an opencv Mat
        
        // memcpy(image.data, cached_messages[0], size * sizeof(uchar));
        // image = cv::Mat(height, width, CV_8UC1, cached_messages[0]);

        image_test = cv::Mat(height, width, CV_8UC1, dataX);        
        image = cv::Mat(height, width, CV_8UC1, cached_messages[0]);



        double goal = (loop_count + 1) / fps;
        Seconds elapsed = SteadyClock::now() - begin;
        if (elapsed.count() < goal)
        {
            this_thread::sleep_for(std::chrono::duration<double>(goal - elapsed.count()));
        }

        // display images code here

        // Display the image
        cv::imshow("Grayscale Image", image);
        cv::imshow("Image Test", image_test);

        // nneded for opencv loop
        cv::waitKey(1);


        // check for long frame times
        end_check = std::chrono::high_resolution_clock::now();
        elapsed = end_check - start_check;
        start_check = std::chrono::high_resolution_clock::now();
        if( elapsed.count()  > .04)
            cout << "XXXXXXXXXXXXXXXXXX  " <<  elapsed.count()  << endl ;



        // for debugging
        auto current = SteadyClock::now();
        loop_sd.increment(current);
        // for debugging
        elapsed = current - begin;
        std::ofstream out("server_counter_2_" + comm->port() + ".txt");
        out << "t:" << elapsed.count() << "s" << endl;
        out << "images_rec'd: " << image_count << endl;
        out << "match: " << matched_count << endl;
        out << "mismatch: " << mismatched_count << endl;
        loop_sd.dump(out, "loop");
        out.close();
        // end debugging
    }

    return 0;
}
