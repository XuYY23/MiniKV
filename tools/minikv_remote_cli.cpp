#include "cluster/kv_rpc.h"

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc < 5) {
    std::cerr << "用法: " << argv[0]
              << " <host> <port> put <key> <value>\n"
              << "      " << argv[0] << " <host> <port> get <key>\n"
              << "      " << argv[0] << " <host> <port> delete <key>\n";
    return 1;
  }
  minikv::cluster::RemoteClusterClient client(
      argv[1], static_cast<uint16_t>(std::atoi(argv[2])));
  const std::string command = argv[3];
  minikv::Status s;
  if (command == "put" && argc == 6) {
    s = client.Put(argv[4], argv[5]);
  } else if (command == "get" && argc == 5) {
    std::string value;
    s = client.Get(argv[4], &value);
    if (s.ok()) std::cout << value << "\n";
  } else if (command == "delete" && argc == 5) {
    s = client.Delete(argv[4]);
  } else {
    std::cerr << "参数不完整\n";
    return 1;
  }
  if (!s.ok()) {
    if (s.IsNotFound()) {
      std::cout << "(null)\n";
      return 0;
    }
    std::cerr << s.ToString() << "\n";
    return 1;
  }
  if (command != "get") std::cout << "OK\n";
  return 0;
}
