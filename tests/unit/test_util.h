#pragma once

#include <filesystem>
#include <string>
#include <unistd.h>

// A fresh directory under the system temp dir, removed when the object goes away.
class TempDir
{
  public:
    TempDir()
    {
        std::string tmpl = (std::filesystem::temp_directory_path() / "daisycola-XXXXXX").string();
        path_            = mkdtemp(tmpl.data());
    }
    ~TempDir() { std::filesystem::remove_all(path_); }

    const std::filesystem::path& path() const { return path_; }
    std::string operator/(const std::string& name) const { return (path_ / name).string(); }

  private:
    std::filesystem::path path_;
};
