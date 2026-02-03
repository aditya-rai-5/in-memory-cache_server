#include "../include/common/config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace config
{

     static std::string trim(std::string s)
     {
          auto not_space = [](unsigned char c)
          {
               return !std::isspace(c);
          };

          s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
          s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
          return s;
     }

     Config &Config::instance()
     {
          static Config cfg;
          return cfg;
     }

     bool Config::load(const std::string &path)
     {
          std::ifstream file(path);
          if (!file.is_open())
          {
               return false;
          }

          std::string line;

          while (std::getline(file, line))
          {
               line = trim(line);
               if (line.empty() || line[0] == '#')
               {
                    continue;
               }

               auto pos = line.find('=');
               if (pos == std::string::npos)
               {
                    continue;
               }

               std::string key = trim(line.substr(0, pos));
               std::string val = trim(line.substr(pos + 1));

               data_[key] = val;
          }
          return true;
     }

     std::string Config::get_string(const std::string &key, const std::string &def) const
     {
          auto it = data_.find(key);
          return it != data_.end() ? it->second : def;
     }

     int Config::get_int(const std::string &key, const int def) const
     {
          auto it = data_.find(key);
          if (it == data_.end())
          {
               return def;
          }
          return std::stoi(it->second);
     }

     bool Config::get_bool(const std::string &key, bool def) const
     {
          auto it = data_.find(key);
          if (it == data_.end())
          {
               return def;
          }
          const std::string &v = it->second;
          return (v == "1" || v == "true" || v == "yes" || v == "on");
     }
}
