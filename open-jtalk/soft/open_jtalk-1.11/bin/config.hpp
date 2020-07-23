#include <unordered_map>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
inline static std::unordered_map<std::string, std::string> config(std::string configFile){
    std::ifstream infile(configFile);
    std::string line;
    std::unordered_map<std::string, std::string> properties;

    while (getline(infile, line)) {
        //todo: trim start of line
        if (line.length() > 0 && line[0] != '#') {
            std::istringstream lineStream(line);
            std::string key, value;
            char delim;
            if ((lineStream >> key >> delim >> value) && (delim == '=')) {
                properties[key] = value;
            }
        }
    }
    return properties;
}