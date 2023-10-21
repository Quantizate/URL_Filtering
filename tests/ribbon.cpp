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

int data_size = 0, test_size, bogus_size = 1000000, query_size, filter_size;

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

  test_size = filter_size = convert(argv[2]);
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

  // DONT CHECK THIS CAUTION CUZ THIS WILL SORT AND FURTHER MESS UP VALUES
  // std::sort(hashes.begin(), hashes.end());
  // auto dup = std::adjacent_find(hashes.begin(), hashes.end());
  // size_t dup_hashes = 0;
  // while (dup != hashes.end())
  // {
  //   dup_hashes++;
  //   dup = std::adjacent_find(dup + 1, hashes.end());
  // }
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

  // printf("-------------- HomogRibbon64_5 --------------\n");
  /*******************************
   * Let us benchmark the filter!
   ******************************/
  /**
   * A filter is a simple data structure that can be easily serialized (e.g., to disk).
   * https://github.com/FastFilter/xor_singleheader#persistent-usage
   */
  // HomogRibbonFilter<uint64_t, 5> hr5(data_size), hr5_test(data_size);

  // // Construction
  // hr5.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = hr5.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings
  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = 1 - hr5.Contain(bogus_hashes[i]);
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
  //              bench([&hashes, &hr5, &dataValidity]()
  //                    {
  //                for (int i = 0; i < hashes.size(); i++)
  //                {
  //                  dataValidity[i].second = hr5.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // printf("Benchmarking construction speed\n");
  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &hr5_test, &size]()
  //                    { hr5_test.AddAll(test_hashes, 0, size); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = 1 - hr5.Contain(hashes[i]);
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

  // printf("-------------- HomogRibbon64_7 --------------\n");
  // HomogRibbonFilter<uint64_t, 7> hr7(test_size);

  // // Construction
  // hr7.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = hr7.SizeInBytes();
  // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  //        100.0 * filter_volume / bytes);
  // printf("\nfilter memory usage : %1.f bits/entry\n",
  //        8.0 * filter_volume / test_hashes.size());
  // printf("\n");
  // // Let us test the query with bogus strings

  // fpp = 0;
  // for (auto &ref : bogus_hashes)
  // {
  //   bool in_set = hr7.Contain(ref);
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
  //   dataValidity[i].second = hr7.Contain(hashes[i]);
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

  // printf("-------------- HomogRibbon64_13 --------------\n");
  // HomogRibbonFilter<uint64_t, 13> hr13(test_size);

  // // Construction
  // hr13.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = hr13.SizeInBytes();
  // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  //        100.0 * filter_volume / bytes);
  // printf("\nfilter memory usage : %1.f bits/entry\n",
  //        8.0 * filter_volume / test_hashes.size());
  // printf("\n");
  // // Let us test the query with bogus strings

  // fpp = 0;
  // for (auto &ref : bogus_hashes)
  // {
  //   bool in_set = hr13.Contain(ref);
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
  //   dataValidity[i].second = hr13.Contain(hashes[i]);
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

  // printf("-------------- HomogRibbon64_15 --------------\n");
  // HomogRibbonFilter<uint64_t, 15> hr15(test_size), hr15_test(test_size);

  // // Construction
  // hr15.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = hr15.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings
  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = 1 - hr15.Contain(bogus_hashes[i]);
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
  //              bench([&hashes, &hr15, &dataValidity]()
  //                    {
  //                for (int i = 0; i < hashes.size(); i++)
  //                {
  //                  dataValidity[i].second = hr15.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // printf("Benchmarking construction speed\n");
  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &hr15_test, &size]()
  //                    { hr15_test.AddAll(test_hashes, 0, size); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = 1 - hr15.Contain(hashes[i]);
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

  // printf("-------------- BalancedRibbon64Pack_5 --------------\n");
  BalancedRibbonFilter<uint64_t, 5, 0> br5(test_size), br5_test(test_size);

  // Construction
  br5.AddAll(test_hashes, 0, test_size);

  // Let us check the size of the filter in bytes:
  filter_volume = br5.SizeInBytes();
  // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  //        100.0 * filter_volume / bytes);
  // printf("\nfilter memory usage : %1.f bits/entry\n",
  //        8.0 * filter_volume / test_hashes.size());
  // printf("\n");

  // Let us test the query with bogus strings
  fp_bogus = 0;
  for (int i = 0; i < bogus_size; i++)
  {
    bool in_set = 1 - br5.Contain(bogus_hashes[i]);
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
               bench([&hashes, &br5, &dataValidity]()
                     {
                 for (int i = 0; i < hashes.size(); i++)
                 {
                   dataValidity[i].second = br5.Contain(hashes[i]);
                 } }),
               stat2);

  // printf("Benchmarking construction speed\n");
  pretty_print(test_hashes.size(), bytes,
               bench([&test_hashes, &br5_test, &size]()
                     { br5_test.AddAll(test_hashes, 0, size); }),
               stat2);

  fprintf(stat2, "\n");

  writeStat1(volume, bytes, filter_volume, stat1);

  // Testing
  for (int i = 0; i < data_size; i++)
  {
    dataValidity[i].second = 1 - br5.Contain(hashes[i]);
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

  // printf("-------------- BalancedRibbon64Pack_7 --------------\n");
  // BalancedRibbonFilter<uint64_t, 7, 0> br7(test_size);

  // // Construction
  // br7.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = br7.SizeInBytes();
  // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  //        100.0 * filter_volume / bytes);
  // printf("\nfilter memory usage : %1.f bits/entry\n",
  //        8.0 * filter_volume / test_hashes.size());
  // printf("\n");
  // // Let us test the query with bogus strings

  // fpp = 0;
  // for (auto &ref : bogus_hashes)
  // {
  //   bool in_set = br7.Contain(ref);
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
  //   dataValidity[i].second = br7.Contain(hashes[i]);
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

  // printf("-------------- BalancedRibbon64Pack_13 --------------\n");
  // BalancedRibbonFilter<uint64_t, 13, 0> br13(test_size);

  // // Construction
  // br13.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = br13.SizeInBytes();
  // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  //        100.0 * filter_volume / bytes);
  // printf("\nfilter memory usage : %1.f bits/entry\n",
  //        8.0 * filter_volume / test_hashes.size());
  // printf("\n");
  // // Let us test the query with bogus strings

  // fpp = 0;
  // for (auto &ref : bogus_hashes)
  // {
  //   bool in_set = br13.Contain(ref);
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
  //   dataValidity[i].second = br13.Contain(hashes[i]);
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

  // printf("-------------- BalancedRibbon64Pack_15 --------------\n");
  // BalancedRibbonFilter<uint64_t, 15, 0> br15(test_size), br15_test(test_size);

  // // Construction
  // br15.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = br15.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings
  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = 1 - br15.Contain(bogus_hashes[i]);
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
  //              bench([&hashes, &br15, &dataValidity]()
  //                    {
  //                for (int i = 0; i < hashes.size(); i++)
  //                {
  //                  dataValidity[i].second = br15.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // printf("Benchmarking construction speed\n");
  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &br15_test, &size]()
  //                    { br15_test.AddAll(test_hashes, 0, size); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = 1 - br15.Contain(hashes[i]);
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

  /// working

  // printf("-------------- StandardRibbon64_25PctPad_7 --------------\n");
  // StandardRibbonFilter<uint64_t, 7, 25> sr7_25(test_size), sr7_25_test(test_size);

  // // Construction
  // sr7_25.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = sr7_25.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings
  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = 1 - sr7_25.Contain(bogus_hashes[i]);
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
  //              bench([&hashes, &sr7_25, &dataValidity]()
  //                    {
  //                for (int i = 0; i < hashes.size(); i++)
  //                {
  //                  dataValidity[i].second = sr7_25.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // printf("Benchmarking construction speed\n");
  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &sr7_25_test, &size]()
  //                    { sr7_25_test.AddAll(test_hashes, 0, size); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = 1 - sr7_25.Contain(hashes[i]);
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

  // printf("-------------- StandardRibbon64_10PctPad_7 --------------\n");
  // BalancedRibbonFilter<uint64_t, 7, 10> sr7_10(test_size), sr7_10_test(test_size);

  // // Construction
  // sr7_10.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = sr7_10.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings
  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = 1 - sr7_10.Contain(bogus_hashes[i]);
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
  //              bench([&hashes, &sr7_10, &dataValidity]()
  //                    {
  //                for (int i = 0; i < hashes.size(); i++)
  //                {
  //                  dataValidity[i].second = sr7_10.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // printf("Benchmarking construction speed\n");
  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &sr7_10_test, &size]()
  //                    { sr7_10_test.AddAll(test_hashes, 0, size); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = 1 - sr7_10.Contain(hashes[i]);
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

  // printf("-------------- StandardRibbon64_15 --------------\n");
  BalancedRibbonFilter<uint64_t, 15, 0> sr15(test_size), sr15_test(test_size);

  // Construction
  sr15.AddAll(test_hashes, 0, test_size);

  // Let us check the size of the filter in bytes:
  filter_volume = sr15.SizeInBytes();
  // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  //        100.0 * filter_volume / bytes);
  // printf("\nfilter memory usage : %1.f bits/entry\n",
  //        8.0 * filter_volume / test_hashes.size());
  // printf("\n");

  // Let us test the query with bogus strings
  fp_bogus = 0;
  for (int i = 0; i < bogus_size; i++)
  {
    bool in_set = 1 - sr15.Contain(bogus_hashes[i]);
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
               bench([&hashes, &sr15, &dataValidity]()
                     {
                 for (int i = 0; i < hashes.size(); i++)
                 {
                   dataValidity[i].second = sr15.Contain(hashes[i]);
                 } }),
               stat2);

  // printf("Benchmarking construction speed\n");
  pretty_print(test_hashes.size(), bytes,
               bench([&test_hashes, &sr15_test, &size]()
                     { sr15_test.AddAll(test_hashes, 0, size); }),
               stat2);

  fprintf(stat2, "\n");

  writeStat1(volume, bytes, filter_volume, stat1);

  // Testing
  for (int i = 0; i < data_size; i++)
  {
    dataValidity[i].second = 1 - sr15.Contain(hashes[i]);
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