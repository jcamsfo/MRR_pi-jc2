#ifndef MIXER_PROCESSOR_H
#define MIXER_PROCESSOR_H

#include <opencv2/opencv.hpp>
#include <vector>
#include <string>


// Load a grayscale image
cv::Mat loadImage(const std::string& imageFile);

// Generate grayscale noise frames
std::vector<cv::Mat> generateNoiseFrames(int width, int height, int numFrames, bool applyFilter);

// Create a parabolic lookup table
cv::Mat createParabolicLUT();

// Function to blend images and noise, and apply LUT
void blendImagesAndNoise(const cv::Mat& img1, const cv::Mat& img2, const std::vector<cv::Mat>& noiseFrames, 
                         cv::Mat& resultImg, const cv::Mat& lut, float imageBlendWeight, 
                         float noiseWeight, float outputGain);

#endif // MIXER_PROCESSOR_H