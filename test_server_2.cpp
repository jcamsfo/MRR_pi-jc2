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
#include "mixer_processor.h"

// #include <pthread.h>

#include "comms.h"

#define APPLY_LOW_PASS_FILTER true // low pass filter the noise Set to false to disable low-pass filtering

#define NUM_OF_NOISE_FRAMES 30

#define FULLSCREEN_MODE false // Set to false for windowed mode

#define SHOW_TIMING true // Show timing on screen

#define NOISE_WEIGHT .6 // 0.5 // Adjust this value as needed (e.g., 0.33 for 1/3)

#define OUTPUT_GAIN 1.8 // Adjust this value for output gain of the final image

#define FADE_TIMER_TC 64  // Adjust this value for lenngth of fade

#define FADE_TIME 38  // Adjust this value for lenngth of fade  nominal 38 frames


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
    int Fade_Timer = 0;
    // int Fade_Timer_TC = 64; //  at  30 fps  64/30 seconds
    // int Fade_Time = 38;    
    bool New_Image = false;
    float Fade_Val = 0;

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
    cv::Mat image1(height, width, CV_8UC1);      // Create an empty cv::Mat with the desired dimensions
    cv::Mat image2(height, width, CV_8UC1);      // Create an empty cv::Mat with the desired dimensions
    // cv::Mat image_mixed(height, width, CV_8UC1); // Create an empty cv::Mat with the desired dimensions
    // cv::Mat image_test(height, width, CV_8UC1);  // Create an empty cv::Mat with the desired dimensions
    cv::Mat transformedImg(height, width, CV_8UC1);  // Create an empty cv::Mat with the desired dimensions
    

    // memcpy(image1.data, dataX, size * sizeof(uchar));      // Copy the data from the 1D array to the cv::Mat
    // memcpy(image2.data, dataX, size * sizeof(uchar));      // Copy the data from the 1D array to the cv::Mat
    // memcpy(image_test.data, dataX, size * sizeof(uchar));  // Copy the data from the 1D array to the cv::Mat
    // memcpy(image_mixed.data, dataX, size * sizeof(uchar)); // Copy the data from the 1D array to the cv::Mat

    // creat 2 empty Mats
    // cv::Mat image, image_test;

    // for timing various things
    auto start_check = std::chrono::high_resolution_clock::now();
    auto end_check = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_check - start_check;

    auto start_check_2 = std::chrono::high_resolution_clock::now();
    auto end_check_2 = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_2 = end_check_2 - start_check_2;

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




    // generate noise
    std::vector<cv::Mat> noiseFrames = generateNoiseFrames(image1.cols, image1.rows, NUM_OF_NOISE_FRAMES, APPLY_LOW_PASS_FILTER);
    //  Create the parabolic lookup table for gamma correction
    cv::Mat lut = createParabolicLUT();

        if (FULLSCREEN_MODE)
    {
        cv::namedWindow("Blended Image Playback", cv::WINDOW_NORMAL);
        cv::setWindowProperty("Blended Image Playback", cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN); // Set window to fullscreen
    }


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

                New_Image = true;

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



        if (New_Image)
        {
            Fade_Timer = 0;
            // image2 = image1.clone();
            memcpy(image2.data, cached_messages[0]->image_data.c_str(), size * sizeof(uchar));
            memcpy(image1.data, cached_messages[1]->image_data.c_str(), size * sizeof(uchar));
            New_Image = false;
            Fade_Val = 0;
            // cout << " NEEEEEEEEEEEEEEEEEEEEEEEWWWWWWWW " << endl;
        }
        else
        {
            if (Fade_Timer < FADE_TIMER_TC)
            {
                Fade_Timer++;
                Fade_Val = (float)(Fade_Timer <= FADE_TIME ? Fade_Timer : FADE_TIME) / (float)FADE_TIME;
            }
        }

        // cv::addWeighted(image2, 1.0 - Fade_Val, image1, Fade_Val, 0, image_mixed);

        blendImagesAndNoise(image1, image2, noiseFrames, transformedImg, lut, Fade_Val, NOISE_WEIGHT, OUTPUT_GAIN);

        end_check_2 = std::chrono::high_resolution_clock::now();
        elapsed_2 = end_check_2 - start_check_2;
        cout << "elapsed_2: " <<  elapsed_2.count() << endl ;


        // Loop Timer to set frame rate
        double goal = (loop_count + 1) / fps;
        Seconds elapsed = SteadyClock::now() - begin;
        if (elapsed.count() < goal)
        {
            this_thread::sleep_for(std::chrono::duration<double>(goal - elapsed.count()));
        }



        start_check_2 = std::chrono::high_resolution_clock::now();     



        // display images code here

        // Display the image
        // cv::imshow("Grayscale Image 1", image1);
        // cv::imshow("Grayscale Image 2", image2);
        // cv::imshow("Grayscale Image 3", image_mixed);
        cv::imshow("Grayscale Image 3", transformedImg);   


        

        

        // cv::imshow("Grayscale Image 3", image_mixed);

        // nneded for opencv loop
        int key = cv::waitKey(1);
        if (key == 27)
        { // ASCII code for the escape key
            break;
        }







        // check for long frame times
        end_check = std::chrono::high_resolution_clock::now();
        elapsed = end_check - start_check;
        start_check = std::chrono::high_resolution_clock::now();
        if (elapsed.count() > .04)
            cout << "XXXXXXXXXXXXXXXXXX  " << elapsed.count() << endl;

        // cout << "XYYYYY " <<  elapsed_2.count()  << endl ;

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
