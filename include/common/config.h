#pragma once

#include <string>
#include <unordered_map>

namespace config
{
     class Config
     {
     public:
          static Config &instance();
          bool load(const std::string &path);

          std::string get_string(const std::string &key,
                                 const std::string &def = "") const;

          int get_int(const std::string &key, int def = 0)
              const;

          bool get_bool(const std::string &key, bool def = false)
              const;

     private:
          Config() = default;
          Config(const Config &) = delete;
          Config &operator=(const Config &) = delete;

     private:
          std::unordered_map<std::string, std::string> data_;
     };
};