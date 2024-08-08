#include "mixer_processor.h"
#include <cmath>
#include <iostream>

cv::Mat loadImage(const std::string& imageFile) {
    cv::Mat img = cv::imread(imageFile, cv::IMREAD_GRAYSCALE);
    if (img.empty()) {
        std::cerr << "Couldn't open image " << imageFile << ".\n";
        exit(-1);
    }
    return img;
}


std::vector<cv::Mat> generateNoiseFrames(int width, int height, int numFrames, bool applyFilter) {
    std::vector<cv::Mat> noiseFrames;
    for (int i = 0; i < numFrames; ++i) {
        cv::Mat noise(height, width, CV_8UC1);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                float value = static_cast<float>(rand() % 256);
                noise.at<uchar>(y, x) = static_cast<uchar>(value);
            }
        }
        if (applyFilter) {
            cv::GaussianBlur(noise, noise, cv::Size(5, 5), 0);

            // Transform the noise
            noise.convertTo(noise, CV_32F); // Convert to float for transformation
            noise -= 128; // Subtract 128
            noise *= 2;   // Multiply by 2
            noise += 128; // Add 128 back
            noise.convertTo(noise, CV_8UC1); // Convert back to 8-bit
        }
        noiseFrames.push_back(noise);
    }
    return noiseFrames;
}

cv::Mat createParabolicLUT() {
    cv::Mat lut(1, 256, CV_8UC1);
    for (int i = 0; i < 256; ++i) {
        float normalized = i / 255.0f;
        lut.at<uchar>(i) = static_cast<uchar>(std::round(255.0f * normalized * normalized));
    }
    return lut;
}



void blendImagesAndNoise(const cv::Mat& img1, const cv::Mat& img2, const std::vector<cv::Mat>& noiseFrames,
                         cv::Mat& outputImg, const cv::Mat& lut,
                         float img1Weight, float noiseWeight, float gain) {
    float img2Weight = 1.0f - img1Weight;
    
    // Determine the current noise frame
    static int noiseFrameIndex = 0;
    cv::Mat noiseFrame = noiseFrames[noiseFrameIndex];
    noiseFrameIndex = (noiseFrameIndex + 1) % noiseFrames.size();
    
    // Blend images
    cv::Mat blendedImage;
    cv::addWeighted(img1, img1Weight, img2, img2Weight, 0, blendedImage);
    
    // Blend noise with the image
    cv::Mat blendedWithNoise;
    cv::addWeighted(blendedImage, 1.0f - noiseWeight, noiseFrame, noiseWeight, 0, blendedWithNoise);
    
    // Apply parabolic LUT
    cv::Mat lutApplied;
    cv::LUT(blendedWithNoise, lut, lutApplied);
    
    // Apply gain directly
    lutApplied.convertTo(outputImg, -1, gain, 0);
}