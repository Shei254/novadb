add_executable(ldb_novadb ldb_novadb.cpp)
target_link_libraries(ldb_novadb record rocksdb "-static -static-libgcc -static-libstdc++ -Wl,--no-as-needed -Wl,--whole-archive -ldl -Wl,--no-whole-archive -pthread -mno-avx -mno-avx2")
