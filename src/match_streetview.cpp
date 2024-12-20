#include <Eigen/Dense>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <opencv2/core/eigen.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <cmath>

using json = nlohmann::json;

#include "vps_slam/match_streetview.hpp"

double tot_start_time;
/**
 * @brief The MatchGoogleStreetView class represents a client for querying Google Street View images.
 * 
 * This class provides methods to query the Google Street View API and retrieve images based on GPS coordinates.
 * It also allows setting parameters such as latitude, longitude, and radius for the query.
 */
MatchGoogleStreetView::MatchGoogleStreetView() 
    : gps_lat(0.0)
    , gps_long(0.0)
    , has_streetview_image_(false) {
}

void MatchGoogleStreetView::SetGPSCoordinates(double lat, double lon) {
    gps_lat = lat;
    gps_long = lon;
    has_streetview_image_ = false;  // Reset flag when GPS changes
}

size_t MatchGoogleStreetView::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    std::vector<unsigned char>* mem = (std::vector<unsigned char>*)userp;
    mem->insert(mem->end(), (unsigned char*)contents, (unsigned char*)contents + realsize);
    return realsize;
}

size_t MatchGoogleStreetView::MetadataCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

MatchGoogleStreetView::StreetViewMetadata MatchGoogleStreetView::QueryMetadata() {
    StreetViewMetadata metadata;
    metadata.available = false;

    std::string serverUrl = "https://maps.googleapis.com/maps/api/streetview/metadata";
    std::string apiKey = "YOUR_API_KEY";  // Replace with your API key
    
    std::string fullUrl = serverUrl + "?location=" + 
                         std::to_string(gps_lat) + "," + 
                         std::to_string(gps_long) + 
                         "&key=" + apiKey;

    CURL *curl;
    CURLcode res;
    std::string readBuffer;
    
    curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, fullUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, MetadataCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if(res == CURLE_OK) {
            ParseMetadataJson(readBuffer, metadata);
        } else {
            RCLCPP_ERROR(rclcpp::get_logger("vps_slam"), 
                        "Failed to get metadata: %s", curl_easy_strerror(res));
        }
    }

    return metadata;
}

bool MatchGoogleStreetView::ParseMetadataJson(const std::string& json_str, 
                                            StreetViewMetadata& metadata) {
    try {
        json j = json::parse(json_str);
        
        if (j["status"] == "OK") {
            metadata.available = true;
            metadata.latitude = j["location"]["lat"].get<double>();
            metadata.longitude = j["location"]["lng"].get<double>();
            metadata.pano_id = j["pano_id"].get<std::string>();
            
            if (j.contains("heading")) {
                metadata.heading = j["heading"].get<double>();
            } else {
                double dx = metadata.longitude - gps_long;
                double dy = metadata.latitude - gps_lat;
                metadata.heading = std::atan2(dx, dy) * 180.0 / M_PI;
            }
            
            RCLCPP_INFO(rclcpp::get_logger("vps_slam"), 
                       "Found Street View image at: %f, %f, heading: %f", 
                       metadata.latitude, metadata.longitude, metadata.heading);
            return true;
        }
    } catch (const std::exception& e) {
        RCLCPP_ERROR(rclcpp::get_logger("vps_slam"), 
                    "Error parsing metadata JSON: %s", e.what());
    }
    return false;
}

cv::Mat MatchGoogleStreetView::QueryStreetViewImage(const StreetViewMetadata& metadata) {
    if (!metadata.available) {
        return cv::Mat();
    }

    std::string serverUrl = "https://maps.googleapis.com/maps/api/streetview";
    std::string apiKey = "YOUR_API_KEY";  // Replace with your API key
    
    std::string fullUrl = serverUrl + "?size=640x480" +
                         "&location=" + std::to_string(metadata.latitude) + 
                         "," + std::to_string(metadata.longitude) +
                         "&heading=" + std::to_string(metadata.heading) +
                         "&fov=90" +
                         "&pitch=0" +
                         "&key=" + apiKey;

    if (!metadata.pano_id.empty()) {
        fullUrl += "&pano=" + metadata.pano_id;
    }

    CURL *curl;
    CURLcode res;
    std::vector<unsigned char> buffer;
    
    curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, fullUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
        res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if(res == CURLE_OK && !buffer.empty()) {
            return cv::imdecode(cv::Mat(buffer), cv::IMREAD_COLOR);
        }
    }
    
    return cv::Mat();
}

cv::Mat MatchGoogleStreetView::GetMatchingPoints(const cv::Mat& img1, const cv::Mat& img2) {
    // Ensure images are not empty
    if (img1.empty() || img2.empty()) {
        std::cerr << "One of the images is empty." << std::endl;
        throw std::runtime_error("Image is empty.");
    }

    // Convert images to grayscale if they are not already
    cv::Mat img1_gray, img2_gray;
    if (img1.channels() == 3) {
        cv::cvtColor(img1, img1_gray, cv::COLOR_BGR2GRAY);
    } else {
        img1_gray = img1.clone();
    }

    if (img2.channels() == 3) {
        cv::cvtColor(img2, img2_gray, cv::COLOR_BGR2GRAY);
    } else {
        img2_gray = img2.clone();
    }
    cv::Ptr<cv::Feature2D> detector = cv::ORB::create();

    double start_time = cv::getTickCount();

    std::vector<cv::KeyPoint> keypoints1;
    cv::Mat descriptors1;
    detector->detectAndCompute(img1, cv::noArray(), keypoints1, descriptors1);

    double end_time = cv::getTickCount();
    double elapsed_time = (end_time - start_time) / cv::getTickFrequency();
    std::cout << "Time to create key points: " << elapsed_time << " seconds" << std::endl;

    cv::Mat frame_with_keypoints = img1.clone();
    cv::drawKeypoints(img1, keypoints1, frame_with_keypoints, cv::Scalar::all(-1), cv::DrawMatchesFlags::DRAW_RICH_KEYPOINTS);

    // cv::imshow("Feature Method - SIFT 1", frame_with_keypoints);

    start_time = cv::getTickCount();

    std::vector<cv::KeyPoint> keypoints2;
    cv::Mat descriptors2;
    detector->detectAndCompute(img2, cv::noArray(), keypoints2, descriptors2);

    end_time = cv::getTickCount();
    elapsed_time = (end_time - start_time) / cv::getTickFrequency();
    std::cout << "Time to create key points: " << elapsed_time << " seconds" << std::endl;

    frame_with_keypoints = img2.clone();
    cv::drawKeypoints(img2, keypoints2, frame_with_keypoints, cv::Scalar::all(-1), cv::DrawMatchesFlags::DRAW_RICH_KEYPOINTS);

    // cv::imshow("Feature Method - SIFT 2", frame_with_keypoints);
    
    start_time = cv::getTickCount();
    cv::BFMatcher matcher;
    std::vector<std::vector<cv::DMatch>> matches;
    matcher.knnMatch(descriptors1, descriptors2, matches, 2);

    std::vector<cv::DMatch> good;
    for (size_t i = 0; i < matches.size(); i++) {
        if (matches[i][0].distance < 0.75 * matches[i][1].distance) {
            good.push_back(matches[i][0]);
        }
    }

    cv::Mat img_matches;
    cv::drawMatches(img1, keypoints1, img2, keypoints2, good, img_matches, cv::Scalar::all(-1), cv::Scalar::all(-1), std::vector<char>(), cv::DrawMatchesFlags::NOT_DRAW_SINGLE_POINTS);

    cv::Mat src_pts(good.size(), 1, CV_32FC2);
    cv::Mat dst_pts(good.size(), 1, CV_32FC2);
    for (size_t i = 0; i < good.size(); i++) {
        src_pts.at<cv::Point2f>(i, 0) = keypoints1[good[i].queryIdx].pt;
        dst_pts.at<cv::Point2f>(i, 0) = keypoints2[good[i].trainIdx].pt;
    }

    // if correspondences are atleast 4, find homography matrix
    if (good.size() >= 4) {
        cv::Mat H = cv::findHomography(src_pts, dst_pts, cv::RANSAC, 5.0);
        std::cout << "Homography matrix: \n" << H << std::endl;
    }
    
    // extract the rotation and translation in the camera frame in meter and radian units
    // std::vector<cv::Mat> rotations, translations, normals;
    // int solutions = cv::decomposeHomographyMat(H, K, rotations, translations, normals);

    // for(int i = 0; i < solutions; ++i) {
    //     std::cout << "Solution " << i << ":\n";
    //     std::cout << "Rotation:\n" << rotations[i] << "\n";
    //     std::cout << "Translation:\n" << translations[i] << "\n";
    //     std::cout << "Normal:\n" << normals[i] << "\n\n";
    // }
    end_time = cv::getTickCount();
    elapsed_time = (end_time - start_time) / cv::getTickFrequency();
    std::cout << "Time to match key points: " << elapsed_time << " seconds" << std::endl;
    return img_matches;
}

int MatchGoogleStreetView::retrieve(double gps_lat, double gps_long, double roi_radius, cv::Mat& image_cam) {
    tot_start_time = cv::getTickCount();

    double start_time = cv::getTickCount();
    cv::Mat img1 = GetStreetView(gps_lat, gps_long, roi_radius);
    double end_time = cv::getTickCount();
    double elapsed_time = (end_time - start_time) / cv::getTickFrequency();
    std::cout << "Time to get image: " << elapsed_time << " seconds" << std::endl;

    // gps_lat = 41.3935598;
    // gps_long = 2.19204;

    // std::cout << "Request 2" << std::endl;
    // start_time = cv::getTickCount();
    // cv::Mat img2 = GetStreetView(gps_lat, gps_long, roi_radius);
    // end_time = cv::getTickCount();
    // elapsed_time = (end_time - start_time) / cv::getTickFrequency();
    // std::cout << "Time to get image: " << elapsed_time << " seconds" << std::endl;
    cv::resize(image_cam, image_cam, cv::Size(640, 480));
    cv::Mat img_matches = GetMatchingPoints(img1, image_cam);
    double tot_end_time = cv::getTickCount();
    double tot_elapsed_time = (tot_end_time - tot_start_time) / cv::getTickFrequency();
    std::cout << "Total time: " << tot_elapsed_time << " seconds" << std::endl;
    
    cv::imshow("Matches", img_matches);
    cv::waitKey(0);

    return 0;
}

cv::Mat MatchGoogleStreetView::GetHomography(const cv::Mat& current_image) {
    // First, query metadata to get exact location
    StreetViewMetadata metadata = QueryMetadata();
    
    if (!metadata.available) {
        RCLCPP_WARN(rclcpp::get_logger("vps_slam"), 
                   "No Street View image available at current location");
        return cv::Mat();
    }

    // Get Street View image using metadata
    last_streetview_image_ = QueryStreetViewImage(metadata);
    if (last_streetview_image_.empty()) {
        RCLCPP_ERROR(rclcpp::get_logger("vps_slam"), 
                    "Failed to get Street View image");
        return cv::Mat();
    }

    // Store metadata for pose estimation
    last_metadata_ = metadata;
    
    // Get matching points and compute homography
    return GetMatchingPoints(current_image, last_streetview_image_);
}

cv::Mat MatchGoogleStreetView::GetStreetView(double lat, double lon, double radius) {
    // Store coordinates for later use
    gps_lat = lat;
    gps_long = lon;

    // First get metadata to find exact location
    StreetViewMetadata metadata = QueryMetadata();
    
    if (!metadata.available) {
        RCLCPP_WARN(rclcpp::get_logger("vps_slam"), 
                   "No Street View image available at location: %f, %f", lat, lon);
        return cv::Mat();
    }

    // Get image using metadata
    cv::Mat streetview_img = QueryStreetViewImage(metadata);
    if (streetview_img.empty()) {
        RCLCPP_ERROR(rclcpp::get_logger("vps_slam"), 
                    "Failed to get Street View image");
        return cv::Mat();
    }

    last_streetview_image_ = streetview_img;
    last_metadata_ = metadata;
    has_streetview_image_ = true;

    return streetview_img;
}
