#include "performancecounters/benchmarker.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <stdlib.h>
#include <vector>
#include "filterapi.h"

// using namespace counting_bloomfilter;

// #define DATA_SIZE 1000000
// #define TEST_SIZE 500000
// #define BOGUS_SIZE 1000000
// #define QUERY_SIZE (DATA_SIZE - TEST_SIZE)

int data_size = 0, test_size, bogus_size = 1000000, query_size;

std::string random_string()
{
  auto randchar = []() -> char
  {
    const char charset[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    const size_t max_index = (sizeof(charset) - 1);
    return charset[rand() % max_index];
  };
  size_t desired_length = rand() % 128;
  std::string str(desired_length, 0);
  std::generate_n(str.begin(), desired_length, randchar);
  return str;
}
void pretty_print(size_t input_volume, size_t bytes, // std::string name,
                  event_aggregate agg, FILE *filename)
{
  fprintf(filename, ",%.2f,%.2f,%.2f", bytes / agg.fastest_elapsed_ns(), input_volume * 1000.0 / agg.fastest_elapsed_ns(), agg.fastest_elapsed_ns() / input_volume);
}

// data size, test size, true positive, true negative, false positive, false negative, bogus size, false_pos_bogus, duplicates data
void writeOutput(size_t tp, size_t tn, size_t fp, size_t fn, size_t fp_bogus, int dup_num, FILE *filename)
{
  fprintf(filename, "%d,%d,%zu,%zu,%zu,%zu,%d,%zu,%d\n", data_size, test_size, tp, tn, fp, fn, bogus_size, fp_bogus, dup_num);
}

// data_size, test_size, average_len (bytes/name), total_data_input_volume, test_input_volume, filter_volume, %usage(wrt to test_input_volume), usuage (wrt to test_size[bits / entry])
void writeStat1(size_t input_input_volume, size_t test_input_volume, size_t filter_volume, FILE *filename)
{
  fprintf(filename, "%d,%d,%zu,%.1f,%zu,%zu,%.2f %%,%.1f\n", data_size, test_size, input_input_volume, double(input_input_volume) / data_size, test_input_volume, filter_volume, 100.0 * filter_volume / test_input_volume, 8.0 * filter_volume / test_size);
}

// data_size, test_size, Benchmarking queries[time/entry, GB/s, Ma/s, ns/d], Benchmarking construction[time/entry, GB/s, Ma/s, ns/d]
void writeStat2(FILE *filename)
{
  fprintf(filename, "%d,%d", data_size, test_size);
}

int convert(char *str)
{
  int num = 0;

  for (int i = 0; str[i] != '\0'; i++)
    num = num * 10 + str[i] - '0';
  return num;
}

uint64_t simple_hash(const std::string &line)
{
  uint64_t h = 0;
  for (unsigned char c : line)
  {
    h = (h * 177) + c;
  }
  h ^= line.size();
  return h;
}

int main(int argc, char **argv)
{
  std::vector<std::string> inputs;

  size_t volume = 0;

  if (argc == 1)
  {
    printf("You must pass a list of URLs (one per line). For instance:\n");
    printf("./benchmark data/top-1m.csv  \n");
    return EXIT_FAILURE;
  }
  else
  {
    std::ifstream input(argv[1]);
    if (!input)
    {
      std::cerr << "Could not open " << argv[1] << std::endl;
      exit(EXIT_FAILURE);
    }
    for (std::string line; std::getline(input, line);)
    {
      std::string ref = line;
      ref.erase(std::find_if(ref.rbegin(), ref.rend(),
                             [](unsigned char ch)
                             { return !std::isspace(ch); })
                    .base(),
                ref.end());
      volume += ref.size();
      inputs.push_back(ref);
      data_size++;
    }
    // std::cout << "loaded " << inputs.size() << " names" << std::endl;
    // std::cout << "average length " << double(volume) / inputs.size()
    //           << " bytes/name" << std::endl;
  }
  // printf("\n");

  FILE *data_reliability = fopen(argv[3], "a");
  FILE *stat1 = fopen(argv[4], "a");
  FILE *stat2 = fopen(argv[5], "a");

  test_size = convert(argv[2]);
  query_size = data_size - test_size;

  bool is_ok;
  size_t fp_bogus = 0;
  size_t filter_volume = 0;
  size_t falsePositive = 0, falseNegative = 0, truePositive = 0, trueNegative = 0;
  volatile int basic_num = 0;

  std::vector<std::pair<bool, bool>> dataValidity(data_size, {false, false}); // original, modified

  /* We are going to check for duplicates. If you have too many duplicates, something might be wrong. */

  int dup_num = 0;
  std::sort(inputs.begin(), inputs.end());
  auto dup_str = std::adjacent_find(inputs.begin(), inputs.end());
  while (dup_str != inputs.end())
  {
    dup_str = std::adjacent_find(dup_str + 1, inputs.end());
    dup_num++;
  }
  size_t bytes = 0;
  for (int i = 0; i < test_size; i++)
  {
    bytes += inputs[i].size();
  }
  // printf("total volume %zu bytes\n", bytes);
  /* We are going to test our hash function to make sure that it is sane. */

  // hashes is *temporary* and does not count in the memory budget
  std::vector<uint64_t> test_hashes(test_size), hashes(data_size), bogus_hashes(bogus_size);
  for (size_t i = 0; i < data_size; i++)
  {
    hashes[i] = simple_hash(inputs[i]);
    if (i < test_size)
    {
      test_hashes[i] = hashes[i];
      dataValidity[i].first = true;
    }
  }

  size_t size = test_hashes.size();

  std::vector<std::string> query_set_bogus;

  for (size_t i = 0; i < bogus_size; i++)
  {
    query_set_bogus.push_back(random_string());
  }

  for (int i = 0; i < bogus_size; i++)
  {
    bogus_hashes[i] = simple_hash(query_set_bogus[i]);
  }

  // printf("\n");
  // printf("Test size(added to filter): %d \n", test_size);
  // printf("Query size(not added to filter): %d \n", query_size);
  // printf("Bogus size(randomly generated strings): %d \n", bogus_size);
  // printf("\n");

  // printf("-------------- Cuckoo - 8 Filter --------------\n");
  /*******************************
   * Let us benchmark the filter!
   ******************************/
  /**
   * A filter is a simple data structure that can be easily serialized (e.g., to disk).
   * https://github.com/FastFilter/xor_singleheader#persistent-usage
   */
  // // Memory allocation (trivial):
  // CuckooFilterStable<uint64_t, 8> filter(test_size), filter_test(test_size);

  // // Construction
  // for (int i = 0; i < test_size; i++)
  // {
  //   filter.Add(test_hashes[i]);
  // }

  // // Let us check the size of the filter in bytes:
  // filter_volume = filter.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings
  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = 1 - filter.Contain(bogus_hashes[i]);
  //   if (in_set)
  //   {
  //     fp_bogus++;
  //   }
  // }

  // // printf("Bogus false-positives: %zu\n", fpp);
  // // printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // // printf("Benchmarking queries:\n");
  // writeStat2(stat2);

  // // printf("Benchmarking queries:\n");

  // pretty_print(inputs.size(), bytes,
  //              bench([&hashes, &filter, &dataValidity]()
  //                    {
  //                for (int i = 0; i < data_size; i++)
  //                {
  //                  dataValidity[i].second = filter.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // printf("Benchmarking construction speed\n");
  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &filter_test, &basic_num]()
  //                    {
  //                    for(basic_num = 0; basic_num < test_hashes.size(); basic_num++){
  //                       filter_test.Add(test_hashes[basic_num]);
  //                       basic_num++; } }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = 1 - filter.Contain(hashes[i]);
  // }

  // falsePositive = 0;
  // falseNegative = 0;
  // truePositive = 0;
  // trueNegative = 0;

  // for (int i = 0; i < data_size; i++)
  // {
  //   if (dataValidity[i].first == true && dataValidity[i].second == true)
  //   {
  //     truePositive++;
  //   }
  //   else if (dataValidity[i].first == true && dataValidity[i].second == false)
  //   {
  //     falseNegative++;
  //   }
  //   else if (dataValidity[i].first == false && dataValidity[i].second == true)
  //   {
  //     falsePositive++;
  //   }
  //   else if (dataValidity[i].first == false && dataValidity[i].second == false)
  //   {
  //     trueNegative++;
  //   }
  // }

  // // printf("\n");
  // // printf("Tested with total data set (test + query): %d \n", data_size);
  // // printf("True Positive: %zu", truePositive);
  // // printf("\t");
  // // printf("True Negative: %zu", trueNegative);
  // // printf("\n");
  // // printf("False Positive: %zu", falsePositive);
  // // printf("\t");
  // // printf("False Negative: %zu", falseNegative);
  // // printf("\n");
  // // printf("\n");

  // writeOutput(truePositive, trueNegative, falsePositive, falseNegative, fp_bogus, dup_num, data_reliability);

  // printf("-------------- Cuckoo - 12 Filter --------------\n");
  // CuckooFilterStable<uint64_t, 12> filter2(test_size);

  // for (int i = 0; i < test_size; i++)
  // {
  //   filter2.Add(test_hashes[i]);
  // }

  // // Let us check the size of the filter in bytes:
  // filter_volume = filter2.SizeInBytes();
  // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  //        100.0 * filter_volume / bytes);
  // printf("\nfilter memory usage : %1.f bits/entry\n",
  //        8.0 * filter_volume / test_hashes.size());
  // printf("\n");
  // // Let us test the query with bogus strings

  // fpp = 0;
  // for (auto &ref : bogus_hashes)
  // {
  //   bool in_set = 1 - filter2.Contain(ref);
  //   if (in_set)
  //   {
  //     fpp++;
  //   }
  // }

  // printf("Bogus false-positives: %zu\n", fpp);
  // printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = 1 - filter2.Contain(hashes[i]);
  // }

  // falsePositive = 0;
  // falseNegative = 0;
  // truePositive = 0;
  // trueNegative = 0;

  // for (int i = 0; i < data_size; i++)
  // {
  //   if (dataValidity[i].first == true && dataValidity[i].second == true)
  //   {
  //     truePositive++;
  //   }
  //   else if (dataValidity[i].first == true && dataValidity[i].second == false)
  //   {
  //     falseNegative++;
  //   }
  //   else if (dataValidity[i].first == false && dataValidity[i].second == true)
  //   {
  //     falsePositive++;
  //   }
  //   else if (dataValidity[i].first == false && dataValidity[i].second == false)
  //   {
  //     trueNegative++;
  //   }
  // }

  // printf("\n");
  // printf("Tested with total data set (test + query): %d \n", data_size);
  // printf("True Positive: %zu", truePositive);
  // printf("\t");
  // printf("True Negative: %zu", trueNegative);
  // printf("\n");
  // printf("False Positive: %zu", falsePositive);
  // printf("\t");
  // printf("False Negative: %zu", falseNegative);
  // printf("\n");
  // printf("\n");

  // writeOutput("Cuckoo-12", truePositive, trueNegative, falsePositive, falseNegative, outputFile, fpp);

  // printf("-------------- Cuckoo - 16 Filter --------------\n");
  // CuckooFilterStable<uint64_t, 16> filter3(test_size), filter3_test(test_size);

  // // Construction
  // for (int i = 0; i < test_size; i++)
  // {
  //   filter3.Add(test_hashes[i]);
  // }

  // // Let us check the size of the filter in bytes:
  // filter_volume = filter3.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings
  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = 1 - filter3.Contain(bogus_hashes[i]);
  //   if (in_set)
  //   {
  //     fp_bogus++;
  //   }
  // }

  // // printf("Bogus false-positives: %zu\n", fpp);
  // // printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // // printf("Benchmarking queries:\n");
  // writeStat2(stat2);

  // // printf("Benchmarking queries:\n");

  // pretty_print(inputs.size(), bytes,
  //              bench([&hashes, &filter3, &dataValidity]()
  //                    {
  //                for (int i = 0; i < data_size; i++)
  //                {
  //                  dataValidity[i].second = filter3.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // printf("Benchmarking construction speed\n");
  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &filter3_test, &basic_num]()
  //                    {
  //                    for(basic_num = 0; basic_num < test_hashes.size(); basic_num++){
  //                       filter3_test.Add(test_hashes[basic_num]);
  //                       basic_num++; } }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = 1 - filter3.Contain(hashes[i]);
  // }

  // falsePositive = 0;
  // falseNegative = 0;
  // truePositive = 0;
  // trueNegative = 0;

  // for (int i = 0; i < data_size; i++)
  // {
  //   if (dataValidity[i].first == true && dataValidity[i].second == true)
  //   {
  //     truePositive++;
  //   }
  //   else if (dataValidity[i].first == true && dataValidity[i].second == false)
  //   {
  //     falseNegative++;
  //   }
  //   else if (dataValidity[i].first == false && dataValidity[i].second == true)
  //   {
  //     falsePositive++;
  //   }
  //   else if (dataValidity[i].first == false && dataValidity[i].second == false)
  //   {
  //     trueNegative++;
  //   }
  // }

  // // printf("\n");
  // // printf("Tested with total data set (test + query): %d \n", data_size);
  // // printf("True Positive: %zu", truePositive);
  // // printf("\t");
  // // printf("True Negative: %zu", trueNegative);
  // // printf("\n");
  // // printf("False Positive: %zu", falsePositive);
  // // printf("\t");
  // // printf("False Negative: %zu", falseNegative);
  // // printf("\n");
  // // printf("\n");

  // writeOutput(truePositive, trueNegative, falsePositive, falseNegative, fp_bogus, dup_num, data_reliability);

  // printf("-------------- Cuckoo Fuse - 16 Filter --------------\n");
  CuckooFuseFilter<uint64_t, uint16_t> fuse_16(data_size), fuse16_test(data_size);

  printf("here");
  // Construction
  for (int i = 0; i < test_size; i++)
  {
    fuse_16.Add(test_hashes[i]);
  }

  // Let us check the size of the filter in bytes:
  filter_volume = fuse_16.SizeInBytes();
  // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  //        100.0 * filter_volume / bytes);
  // printf("\nfilter memory usage : %1.f bits/entry\n",
  //        8.0 * filter_volume / test_hashes.size());
  // printf("\n");

  // Let us test the query with bogus strings
  fp_bogus = 0;
  for (int i = 0; i < bogus_size; i++)
  {
    bool in_set = 1 - fuse_16.Contain(bogus_hashes[i]);
    if (in_set)
    {
      fp_bogus++;
    }
  }

  // printf("Bogus false-positives: %zu\n", fpp);
  // printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // printf("Benchmarking queries:\n");
  writeStat2(stat2);

  // printf("Benchmarking queries:\n");

  pretty_print(inputs.size(), bytes,
               bench([&hashes, &fuse_16, &dataValidity]()
                     {
                 for (int i = 0; i < data_size; i++)
                 {
                   dataValidity[i].second = fuse_16.Contain(hashes[i]);
                 } }),
               stat2);

  // printf("Benchmarking construction speed\n");
  pretty_print(test_hashes.size(), bytes,
               bench([&test_hashes, &fuse16_test, &basic_num]()
                     {
                     for(basic_num = 0; basic_num < test_hashes.size(); basic_num++){
                        fuse16_test.Add(test_hashes[basic_num]);
                        } }),
               stat2);

  fprintf(stat2, "\n");

  writeStat1(volume, bytes, filter_volume, stat1);

  // Testing
  for (int i = 0; i < data_size; i++)
  {
    dataValidity[i].second = 1 - fuse_16.Contain(hashes[i]);
  }

  falsePositive = 0;
  falseNegative = 0;
  truePositive = 0;
  trueNegative = 0;

  for (int i = 0; i < data_size; i++)
  {
    if (dataValidity[i].first == true && dataValidity[i].second == true)
    {
      truePositive++;
    }
    else if (dataValidity[i].first == true && dataValidity[i].second == false)
    {
      falseNegative++;
    }
    else if (dataValidity[i].first == false && dataValidity[i].second == true)
    {
      falsePositive++;
    }
    else if (dataValidity[i].first == false && dataValidity[i].second == false)
    {
      trueNegative++;
    }
  }

  // printf("\n");
  // printf("Tested with total data set (test + query): %d \n", data_size);
  // printf("True Positive: %zu", truePositive);
  // printf("\t");
  // printf("True Negative: %zu", trueNegative);
  // printf("\n");
  // printf("False Positive: %zu", falsePositive);
  // printf("\t");
  // printf("False Negative: %zu", falseNegative);
  // printf("\n");
  // printf("\n");

  writeOutput(truePositive, trueNegative, falsePositive, falseNegative, fp_bogus, dup_num, data_reliability);

  fclose(data_reliability);
  fclose(stat1);
  fclose(stat2);
  return EXIT_SUCCESS;
}