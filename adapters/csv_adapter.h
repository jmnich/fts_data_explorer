#pragma once

#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <iostream>

// Data structure to hold interferogram data
struct InterferogramData {
    std::vector<double> referenceDetector;
    std::vector<double> primaryDetector;
    std::string metadata;
};

class CSVAdapter {
public:
    /**
     * Load interferogram data from a CSV file
     * 
     * @param filePath Path to the CSV file
     * @return InterferogramData containing reference and primary detector data
     * @throws std::runtime_error if file cannot be opened
     */
    static InterferogramData loadFromCSV(const std::string& filePath) {
        InterferogramData data;
        std::ifstream file(filePath);
        
        if (!file.is_open()) {
            throw std::runtime_error("Could not open file: " + filePath);
        }
        
        std::string line;
        bool isFirstLine = true;
        
        while (std::getline(file, line)) {
            if (isFirstLine) {
                isFirstLine = false;
                continue; // Skip header line
            }
            
            std::istringstream iss(line);
            std::string refValue, primaryValue;
            
            // Parse CSV line (format: Reference detector [V],Primary detector [V])
            if (std::getline(iss, refValue, ',') && std::getline(iss, primaryValue, ',')) {
                try {
                    // Convert strings to doubles and store in vectors
                    data.referenceDetector.push_back(std::stod(refValue));
                    data.primaryDetector.push_back(std::stod(primaryValue));
                } catch (const std::exception& e) {
                    // Skip malformed lines but continue processing
                    std::cerr << "Warning: Error parsing line '" << line 
                              << "' in file " << filePath << " - " << e.what() << std::endl;
                }
            }
        }
        
        // Add basic metadata
        data.metadata = "CSV File: " + filePath;
        
        return data;
    }
};