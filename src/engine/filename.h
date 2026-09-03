#pragma once

#include <cstdint>
#include <string>

namespace minikv {

// 数据库目录内的文件命名约定（相对 dbname）。

std::string LogFileName(const std::string& dbname, uint64_t number);
std::string TableFileName(const std::string& dbname, uint64_t number);
std::string ManifestFileName(const std::string& dbname);
std::string TempFileName(const std::string& dbname, const std::string& suffix);

// 解析 "000001.sst" 一类文件名中的编号；失败返回 false。
bool ParseFileNumber(const std::string& fname, uint64_t* number);

}  // namespace minikv
