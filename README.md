# URL_Filter Experimentation

Usage:

```
Select the respective cpp file to make
./script.sh data_file_path final_count step output_file1_path output_file2_path output_file3_path
```

Possible output:

```
$ ./script.sh index data/top-1m.csv 1000000 50000 results/data_reliability.csv results/stat1.csv results/stat2.csv

rm -rf index
c++  -o index tests/binary_fuse.cpp -O3 -I src -std=c++17 -Wall -Wextra
Progress : [######----------------------------------] 15.00% (150000/1000000)

```

Upon Completion:

```
Progress : [########################################] 100.00% (1000000/1000000)
DONE
```

## References

Thomas Mueller Graf, Daniel Lemire, [Binary Fuse Filters: Fast and Smaller Than Xor Filters](https://arxiv.org/abs/2201.01174), Journal of Experimental Algorithmics 27, 2022

Fast Filter, GitHub Repository: [link](https://github.com/FastFilter/fastfilter_cpp/tree/master)

URL Filter, GitHub Repository: [link](https://github.com/FastFilter/url_filter/tree/main)
