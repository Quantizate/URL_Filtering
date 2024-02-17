#include "performancecounters/benchmarker.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <stdlib.h>
#include <vector>
#include "filterapi.h"

// #define FILTER_SIZE 500000
// #define DATA_SIZE 1000000
// #define TEST_SIZE 500000
// #define BOGUS_SIZE 1000000
// #define QUERY_SIZE (DATA_SIZE - TEST_SIZE)

int filter_size, data_size = 0, test_size, bogus_size = 1000000, query_size;

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

  size_t input_volume = 0;

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
    input_volume = 0;
    for (std::string line; std::getline(input, line);)
    {
      std::string ref = line;
      ref.erase(std::find_if(ref.rbegin(), ref.rend(),
                             [](unsigned char ch)
                             { return !std::isspace(ch); })
                    .base(),
                ref.end());
      input_volume += ref.size();
      inputs.push_back(ref);
      data_size++;
    }
    // std::cout << "loaded " << inputs.size() << " names" << std::endl;
    // std::cout << "average length " << double(input_volume) / inputs.size()
    //           << " bytes/name" << std::endl;
  }
  // printf("\n");

  FILE *data_reliability = fopen(argv[3], "a");
  FILE *stat1 = fopen(argv[4], "a");
  FILE *stat2 = fopen(argv[5], "a");

  test_size = filter_size = convert(argv[2]);
  query_size = data_size - test_size;

  bool is_ok;
  size_t fp_bogus = 0;
  size_t filter_volume = 0;
  size_t falsePositive = 0, falseNegative = 0, truePositive = 0, trueNegative = 0;
  volatile size_t basic_count = 0;

  std::vector<std::pair<bool, bool>> dataValidity(data_size, {false, false}); // original, modified

  /* We are going to check for duplicates. If you have too many duplicates, something might be wrong. */
  int dup_num = 0;
  std::sort(inputs.begin(), inputs.end());
  auto dup_str = std::adjacent_find(inputs.begin(), inputs.end());
  while (dup_str != inputs.end())
  {
    // std::cout << "duplicated string " << *dup_str << std::endl;
    dup_str = std::adjacent_find(dup_str + 1, inputs.end());
    dup_num++;
  }
  size_t bytes = 0;
  for (int i = 0; i < test_size; i++)
  {
    bytes += inputs[i].size();
  }
  // printf("total input_volume %zu bytes\n", bytes);

  /* We are going to test our hash function to make sure that it is sane. */

  // hashes is *temporary* and does not count in the memory budget
  std::vector<uint64_t> test_hashes(test_size), hashes(data_size), bogus_hashes(bogus_size);
  for (size_t i = 0; i < (size_t)data_size; i++)
  {
    hashes[i] = simple_hash(inputs[i]);
    if (i < (size_t)test_size)
    {
      test_hashes[i] = hashes[i];
      dataValidity[i].first = true;
    }
  }

  // printf("number of duplicates hashes %zu\n", count);
  // printf("ratio of duplicates  hashes %f\n", count / double(test_hashes.size()));

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

  // printf("-------------- Bloom addAll- 8 Filter --------------\n");
  /*******************************
   * Let us benchmark the filter!
   ******************************/
  /**
   * A filter is a simple data structure that can be easily serialized (e.g., to disk).
   * https://github.com/FastFilter/xor_singleheader#persistent-usage
   */
  // Memory allocation (trivial):
  // BloomFilter<uint64_t, 8, false> filter(filter_size), filter_test(filter_size);

  // // Construction
  // filter.AddAll(test_hashes, 0, test_size);

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

  // // printf("Bogus false-positives: %zu\n", fp_bogus);
  // // printf("Bogus false-positive rate %f\n", fp_bogus / double(query_set_bogus.size()));

  // // printf("Benchmarking queries:\n");

  // basic_count = 0;
  // writeStat2(stat2);

  // pretty_print(inputs.size(), bytes,
  //              bench([&hashes, &filter, &basic_count]()
  //                    {
  //                for (int i = 0;i < data_size;i++) {
  //                  basic_count +=
  //                      filter.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // printf("Benchmarking construction speed\n");

  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &filter_test, &size]()
  //                    { filter_test.AddAll(test_hashes, 0, size); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(input_volume, bytes, filter_volume, stat1);

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

  // printf("-------------- Bloom addAll - 12 Filter --------------\n");
  // BloomFilter<uint64_t, 12, false> filter2(filter_size), filter2_test(filter_size);

  // // Construction
  // filter2.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = filter2.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings
  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = 1 - filter2.Contain(bogus_hashes[i]);
  //   if (in_set)
  //   {
  //     fp_bogus++;
  //   }
  // }

  // // printf("Bogus false-positives: %zu\n", fp_bogus);
  // // printf("Bogus false-positive rate %f\n", fp_bogus / double(query_set_bogus.size()));

  // // printf("Benchmarking queries:\n");

  // basic_count = 0;
  // writeStat2(stat2);

  // pretty_print(inputs.size(), bytes,
  //              bench([&hashes, &filter2, &basic_count]()
  //                    {
  //                for (int i = 0;i < data_size;i++) {
  //                  basic_count +=
  //                      filter2.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // printf("Benchmarking construction speed\n");

  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &filter2_test, &size]()
  //                    { filter2_test.AddAll(test_hashes, 0, size); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(input_volume, bytes, filter_volume, stat1);

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

  // // printf("-------------- Bloom addAll - 16 Filter --------------\n");
  // BloomFilter<uint64_t, 16, false> filter3(filter_size), filter3_test(filter_size);

  // // Construction
  // filter3.AddAll(test_hashes, 0, test_size);

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

  // // printf("Bogus false-positives: %zu\n", fp_bogus);
  // // printf("Bogus false-positive rate %f\n", fp_bogus / double(query_set_bogus.size()));

  // // printf("Benchmarking queries:\n");

  // basic_count = 0;
  // writeStat2(stat2);

  // pretty_print(inputs.size(), bytes,
  //              bench([&hashes, &filter3, &basic_count]()
  //                    {
  //                for (int i = 0;i < data_size;i++) {
  //                  basic_count +=
  //                      filter3.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // printf("Benchmarking construction speed\n");

  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &filter3_test, &size]()
  //                    { filter3_test.AddAll(test_hashes, 0, size); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(input_volume, bytes, filter_volume, stat1);

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

  // printf("-------------- Bloom - 8 Filter --------------\n");
  // BloomFilter<uint64_t, 8, false> bloom_8(filter_size);

  // // Construction
  // for (int i = 0; i < test_size; i++)
  // {
  //   bloom_8.Add(hashes[i]);
  // }

  // // Let us check the size of the filter in bytes:
  // filter_volume = bloom_8.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");
  // // Let us test the query with bogus strings

  // fp_bogus = 0;
  // for (auto &ref : bogus_hashes)
  // {
  //   bool in_set = 1 - bloom_8.Contain(ref);
  //   if (in_set)
  //   {
  //     fp_bogus++;
  //   }
  // }

  // // printf("Bogus false-positives: %zu\n", fp_bogus);
  // // printf("Bogus false-positive rate %f\n", fp_bogus / double(query_set_bogus.size()));

  // writeStat2(stat2);

  // // Benchmarking queries:
  // pretty_print(inputs.size(), bytes,
  //              bench([&hashes, &filter, &dataValidity]()
  //                    {
  //                for (int i = 0; i < data_size; i++)
  //                {
  //                  dataValidity[i].second = binary_fuse16_contain(hashes[i], &filter);
  //                } }),
  //              stat2);

  // // Benchmarking construction speed
  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &filter, &size]()
  //                    { binary_fuse16_populate(test_hashes.data(), size, &filter); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = 1 - bloom_8.Contain(hashes[i]);
  // }

  // falsePositive = falseNegative = truePositive = trueNegative = 0;

  // for (int i = 0; i < data_size; i++)
  // {
  //   if (dataValidity[i].first == true && dataValidity[i].second == true)
  //     truePositive++;
  //   else if (dataValidity[i].first == true && dataValidity[i].second == false)
  //     falseNegative++;
  //   else if (dataValidity[i].first == false && dataValidity[i].second == true)
  //     falsePositive++;
  //   else if (dataValidity[i].first == false && dataValidity[i].second == false)
  //     trueNegative++;
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

  // writeOutput(truePositive, trueNegative, falsePositive, falseNegative, fp_bogus, dup_num, dup_hashes, data_reliability);

  // // printf("-------------- Bloom addAll - 32 Filter --------------\n");
  // BloomFilter<uint64_t, 32, false> bl_32(filter_size), bl_32_test(filter_size);

  // // Construction
  // bl_32.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = bl_32.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings
  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = 1 - bl_32.Contain(bogus_hashes[i]);
  //   if (in_set)
  //   {
  //     fp_bogus++;
  //   }
  // }

  // // printf("Bogus false-positives: %zu\n", fp_bogus);
  // // printf("Bogus false-positive rate %f\n", fp_bogus / double(query_set_bogus.size()));

  // // printf("Benchmarking queries:\n");

  // basic_count = 0;
  // writeStat2(stat2);

  // pretty_print(inputs.size(), bytes,
  //              bench([&hashes, &bl_32, &basic_count]()
  //                    {
  //                for (int i = 0;i < data_size;i++) {
  //                  basic_count +=
  //                      bl_32.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // printf("Benchmarking construction speed\n");

  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &bl_32_test, &size]()
  //                    { bl_32_test.AddAll(test_hashes, 0, size); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(input_volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = 1 - bl_32.Contain(hashes[i]);
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

  // // printf("-------------- Bloom addAll - 48 Filter --------------\n");
  BloomFilter<uint64_t, 48, false> bl_48(filter_size), bl_48_test(filter_size);

  // Construction
  bl_48.AddAll(test_hashes, 0, test_size);

  // Let us check the size of the filter in bytes:
  filter_volume = bl_48.SizeInBytes();
  // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  //        100.0 * filter_volume / bytes);
  // printf("\nfilter memory usage : %1.f bits/entry\n",
  //        8.0 * filter_volume / test_hashes.size());
  // printf("\n");

  // Let us test the query with bogus strings
  fp_bogus = 0;
  for (int i = 0; i < bogus_size; i++)
  {
    bool in_set = 1 - bl_48.Contain(bogus_hashes[i]);
    if (in_set)
    {
      fp_bogus++;
    }
  }

  // printf("Bogus false-positives: %zu\n", fp_bogus);
  // printf("Bogus false-positive rate %f\n", fp_bogus / double(query_set_bogus.size()));

  // printf("Benchmarking queries:\n");

  basic_count = 0;
  writeStat2(stat2);

  pretty_print(inputs.size(), bytes,
               bench([&hashes, &bl_48, &basic_count]()
                     {
                 for (int i = 0;i < data_size;i++) {
                   basic_count +=
                       bl_48.Contain(hashes[i]);
                 } }),
               stat2);

  // printf("Benchmarking construction speed\n");

  pretty_print(test_hashes.size(), bytes,
               bench([&test_hashes, &bl_48_test, &size]()
                     { bl_48_test.AddAll(test_hashes, 0, size); }),
               stat2);

  fprintf(stat2, "\n");

  writeStat1(input_volume, bytes, filter_volume, stat1);

  // Testing
  for (int i = 0; i < data_size; i++)
  {
    dataValidity[i].second = 1 - bl_48.Contain(hashes[i]);
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

  // // printf("-------------- Bloom addAll - 24 Filter --------------\n");
  // BloomFilter<uint64_t, 24, false> bl_24(filter_size), bl_24_test(filter_size);

  // // Construction
  // bl_24.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = bl_24.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings
  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = 1 - bl_24.Contain(bogus_hashes[i]);
  //   if (in_set)
  //   {
  //     fp_bogus++;
  //   }
  // }

  // // printf("Bogus false-positives: %zu\n", fp_bogus);
  // // printf("Bogus false-positive rate %f\n", fp_bogus / double(query_set_bogus.size()));

  // // printf("Benchmarking queries:\n");

  // basic_count = 0;
  // writeStat2(stat2);

  // pretty_print(inputs.size(), bytes,
  //              bench([&hashes, &bl_24, &basic_count]()
  //                    {
  //                for (int i = 0;i < data_size;i++) {
  //                  basic_count +=
  //                      bl_24.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // printf("Benchmarking construction speed\n");

  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &bl_24_test, &size]()
  //                    { bl_24_test.AddAll(test_hashes, 0, size); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(input_volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = 1 - bl_24.Contain(hashes[i]);
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

  fclose(data_reliability);
  fclose(stat1);
  fclose(stat2);

  return EXIT_SUCCESS;
}