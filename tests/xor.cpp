#include "performancecounters/benchmarker.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <stdlib.h>
#include <vector>

extern "C"
{
#include "./xorfilter/xorfilter_singleheader.h"
}

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

int convert(char *str)
{
  int num = 0;

  for (int i = 0; str[i] != '\0'; i++)
    num = num * 10 + str[i] - '0';
  return num;
}

void pretty_print(size_t volume, size_t bytes, // std::string name,
                  event_aggregate agg, FILE *filename)
{
  fprintf(filename, ",%.2f,%.2f,%.2f", bytes / agg.fastest_elapsed_ns(), volume * 1000.0 / agg.fastest_elapsed_ns(), agg.fastest_elapsed_ns() / volume);
}

// data size, test size, true positive, true negative, false positive, false negative, bogus size, false_pos_bogus, duplicates data
void writeOutput(size_t tp, size_t tn, size_t fp, size_t fn, size_t fp_bogus, int dup_num, FILE *filename)
{
  fprintf(filename, "%d,%d,%zu,%zu,%zu,%zu,%d,%zu,%d\n", data_size, test_size, tp, tn, fp, fn, bogus_size, fp_bogus, dup_num);
}

// data_size, test_size, total_data_volume, average_len (bytes/name), test_volume, filter_volume, %usage(wrt to test_volume), usuage (wrt to test_size[bits / entry])
void writeStat1(size_t input_volume, size_t test_volume, size_t filter_volume, FILE *filename)
{
  fprintf(filename, "%d,%d,%zu,%.1f,%zu,%zu,%.2f %%,%.1f\n", data_size, test_size, input_volume, double(input_volume) / data_size, test_volume, filter_volume, 100.0 * filter_volume / test_volume, 8.0 * filter_volume / test_size);
}

// data_size, test_size, Benchmarking queries[time/entry, GB/s, Ma/s, ns/d], Benchmarking construction[time/entry, GB/s, Ma/s, ns/d]

void writeStat2(FILE *filename)
{
  fprintf(filename, "%d,%d", data_size, test_size);
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
    size_t volume = 0;
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
  // printf("total volume %zu bytes\n", bytes);

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

  // printf("-------------- Xor - 8 Filter --------------\n");
  // xor8_t filter_8;
  // // // Memory allocation (trivial):
  // is_ok = xor8_allocate(test_size, &filter_8);
  // if (!is_ok)
  // {
  //   printf("You probably ran out of memory. Try a smaller size.\n");
  //   return EXIT_FAILURE;
  // }

  // // Construction
  // is_ok = xor8_populate(test_hashes.data(), test_hashes.size(), &filter_8);
  // if (!is_ok)
  // {
  //   // This cannot happen unless there is a bug in the library or you provided a bad input (e.g., all duplicates).
  //   printf("Construction failed. This should not happen.\n");
  //   return EXIT_FAILURE;
  // }

  // // Let us check the size of the filter in bytes:
  // filter_volume = xor8_size_in_bytes(&filter_8);
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings

  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = xor8_contain(bogus_hashes[i], &filter_8);
  //   if (in_set)
  //   {
  //     fp_bogus++;
  //   }
  // }

  // // printf("Bogus false-positives: %zu\n", fpp);
  // // printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // writeStat2(stat2);

  // // Benchmarking queries:
  // pretty_print(inputs.size(), bytes,
  //              bench([&hashes, &filter_8, &dataValidity]()
  //                    {
  //                for (int i = 0; i < data_size; i++)
  //                {
  //                  dataValidity[i].second = xor8_contain(hashes[i], &filter_8);
  //                } }),
  //              stat2);

  // // Benchmarking construction speed
  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &filter_8, &size]()
  //                    { xor8_populate(test_hashes.data(), size, &filter_8); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = xor8_contain(hashes[i], &filter_8);
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
  // // printf("True Positive: %zu\n", truePositive);
  // // printf("True Negative: %zu\n", trueNegative);
  // // printf("False Positive: %zu\n", falsePositive);
  // // printf("False Negative: %zu\n", falseNegative);

  // writeOutput(truePositive, trueNegative, falsePositive, falseNegative, fp_bogus, dup_num, data_reliability);

  // xor8_free(&filter_8);

  // printf("-------------- Xor - 16 Filter --------------\n");
  xor16_t filter_16;

  // // Memory allocation (trivial):
  is_ok = xor16_allocate(test_size, &filter_16);
  if (!is_ok)
  {
    printf("You probably ran out of memory. Try a smaller size.\n");
    return EXIT_FAILURE;
  }

  // Construction
  is_ok = xor16_populate(test_hashes.data(), test_hashes.size(), &filter_16);
  if (!is_ok)
  {
    // This cannot happen unless there is a bug in the library or you provided a bad input (e.g., all duplicates).
    printf("Construction failed. This should not happen.\n");
    return EXIT_FAILURE;
  }

  // Let us check the size of the filter in bytes:
  filter_volume = xor16_size_in_bytes(&filter_16);
  // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  //        100.0 * filter_volume / bytes);
  // printf("\nfilter memory usage : %1.f bits/entry\n",
  //        8.0 * filter_volume / test_hashes.size());
  // printf("\n");

  // Let us test the query with bogus strings

  fp_bogus = 0;
  for (int i = 0; i < bogus_size; i++)
  {
    bool in_set = xor16_contain(bogus_hashes[i], &filter_16);
    if (in_set)
    {
      fp_bogus++;
    }
  }

  // printf("Bogus false-positives: %zu\n", fpp);
  // printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  writeStat2(stat2);

  // Benchmarking queries:
  pretty_print(inputs.size(), bytes,
               bench([&hashes, &filter_16, &dataValidity]()
                     {
                 for (int i = 0; i < data_size; i++)
                 {
                   dataValidity[i].second = xor16_contain(hashes[i], &filter_16);
                 } }),
               stat2);

  // Benchmarking construction speed
  pretty_print(test_hashes.size(), bytes,
               bench([&test_hashes, &filter_16, &size]()
                     { xor16_populate(test_hashes.data(), size, &filter_16); }),
               stat2);

  fprintf(stat2, "\n");

  writeStat1(volume, bytes, filter_volume, stat1);

  // Testing
  for (int i = 0; i < data_size; i++)
  {
    dataValidity[i].second = xor16_contain(hashes[i], &filter_16);
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
  // printf("True Positive: %zu\n", truePositive);
  // printf("True Negative: %zu\n", trueNegative);
  // printf("False Positive: %zu\n", falsePositive);
  // printf("False Negative: %zu\n", falseNegative);

  writeOutput(truePositive, trueNegative, falsePositive, falseNegative, fp_bogus, dup_num, data_reliability);

  xor16_free(&filter_16);
  fclose(data_reliability);
  fclose(stat1);
  fclose(stat2);
  return EXIT_SUCCESS;
}