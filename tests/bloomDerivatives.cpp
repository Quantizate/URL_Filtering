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

  // printf("-------------- Branchless Bloom- 8 Filter --------------\n");

  // // Memory allocation (trivial):
  // BloomFilter<uint64_t, 8, true> bBloom_8(filter_size), bBloom_8_test(filter_size);

  // // Construction
  // bBloom_8.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = bBloom_8.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings
  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = 1 - bBloom_8.Contain(bogus_hashes[i]);
  //   if (in_set)
  //   {
  //     fp_bogus++;
  //   }
  // }

  // // printf("Bogus false-positives: %zu\n", fpp);
  // // printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // // printf("Benchmarking queries:\n");

  // basic_count = 0;
  // writeStat2(stat2);

  // pretty_print(inputs.size(), bytes,
  //              bench([&hashes, &bBloom_8, &basic_count]()
  //                    {
  //                for (int i = 0;i < data_size;i++) {
  //                  basic_count +=
  //                      bBloom_8.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // printf("Benchmarking construction speed\n");

  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &bBloom_8_test, &size]()
  //                    { bBloom_8_test.AddAll(test_hashes, 0, size); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(input_volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = 1 - bBloom_8.Contain(hashes[i]);
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

  // printf("-------------- Branchless Bloom - 16 Filter --------------\n");
  // BloomFilter<uint64_t, 16, true> bBloom_16(filter_size), bBloom_16_test(filter_size);

  // // Construction
  // bBloom_16.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = bBloom_16.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings
  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = 1 - bBloom_16.Contain(bogus_hashes[i]);
  //   if (in_set)
  //   {
  //     fp_bogus++;
  //   }
  // }

  // // printf("Bogus false-positives: %zu\n", fpp);
  // // printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // // printf("Benchmarking queries:\n");

  // basic_count = 0;
  // writeStat2(stat2);

  // pretty_print(inputs.size(), bytes,
  //              bench([&hashes, &bBloom_16, &basic_count]()
  //                    {
  //                for (int i = 0;i < data_size;i++) {
  //                  basic_count +=
  //                      bBloom_16.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // printf("Benchmarking construction speed\n");

  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &bBloom_16_test, &size]()
  //                    { bBloom_16_test.AddAll(test_hashes, 0, size); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(input_volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = 1 - bBloom_16.Contain(hashes[i]);
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

  // printf("-------------- Simple Blocked Bloom Filter --------------\n");
  // SimpleBlockFilter<8, 16> blockedBloom_16(filter_size), blockedBloom_16_test(filter_size);

  // // Construction
  // for (int i = 0; i < test_size; i++)
  // {
  //   blockedBloom_16.Add(test_hashes[i]);
  // }

  // // Let us check the size of the filter in bytes:
  // filter_volume = blockedBloom_16.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings
  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = 1 - blockedBloom_16.Find(bogus_hashes[i]);
  //   if (in_set)
  //   {
  //     fp_bogus++;
  //   }
  // }

  // // printf("Bogus false-positives: %zu\n", fpp);
  // // printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // // printf("Benchmarking queries:\n");

  // basic_count = 0;
  // writeStat2(stat2);

  // pretty_print(inputs.size(), bytes,
  //              bench([&hashes, &blockedBloom_16, &basic_count]()
  //                    {
  //                for (int i = 0;i < data_size;i++) {
  //                  basic_count +=
  //                      blockedBloom_16.Find(hashes[i]);
  //                } }),
  //              stat2);

  // // printf("Benchmarking construction speed\n");
  // volatile int count = 0;
  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &blockedBloom_16_test, &count]()
  //                    {
  //              for(count = 0;count < test_size;count++) {
  //                 blockedBloom_16_test.Add(test_hashes[count]);
  //               } }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(input_volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = 1 - blockedBloom_16.Find(hashes[i]);
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
  // printf("-------------- Blocked Bloom Filter --------------\n");
  // SimdBlockFilterFixed blockedBloom(filter_size);

  // // Construction
  // for (int i = 0; i < test_size; i++)
  //   blockedBloom.Add(test_hashes[i]);

  // // Let us check the size of the filter in bytes:
  // filter_volume = blockedBloom.SizeInBytes();
  // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  //        100.0 * filter_volume / bytes);
  // printf("\nfilter memory usage : %1.f bits/entry\n",
  //        8.0 * filter_volume / test_hashes.size());
  // printf("\n");
  // // Let us test the query with bogus strings

  // fpp = 0;
  // for (auto &ref : bogus_hashes)
  // {
  //   bool in_set = blockedBloom.Find(ref);
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
  //   dataValidity[i].second = blockedBloom.Find(hashes[i]);
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

  // printf("-------------- Counting Bloom 16 Filter --------------\n");
  // CountingBloomFilter<uint64_t, 16, true> cntBloom16(filter_size), cntBloom16_test(filter_size);

  // // Construction
  // cntBloom16.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = cntBloom16.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings
  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = 1 - cntBloom16.Contain(bogus_hashes[i]);
  //   if (in_set)
  //   {
  //     fp_bogus++;
  //   }
  // }

  // // printf("Bogus false-positives: %zu\n", fpp);
  // // printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // // printf("Benchmarking queries:\n");

  // basic_count = 0;
  // writeStat2(stat2);

  // pretty_print(inputs.size(), bytes,
  //              bench([&hashes, &cntBloom16, &basic_count]()
  //                    {
  //                for (int i = 0;i < data_size;i++) {
  //                  basic_count +=
  //                      cntBloom16.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // printf("Benchmarking construction speed\n");

  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &cntBloom16_test, &size]()
  //                    { cntBloom16_test.AddAll(test_hashes, 0, size); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(input_volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = 1 - cntBloom16.Contain(hashes[i]);
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

  // // printf("-------------- Succinct Counting Bloom 16 Filter --------------\n");
  // SuccinctCountingBloomFilter<uint64_t, 10, true> scountBloom(filter_size), scountBloom_test(filter_size);

  // // Construction
  // scountBloom.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = scountBloom.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings
  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = 1 - scountBloom.Contain(bogus_hashes[i]);
  //   if (in_set)
  //   {
  //     fp_bogus++;
  //   }
  // }

  // // printf("Bogus false-positives: %zu\n", fpp);
  // // printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // // printf("Benchmarking queries:\n");

  // basic_count = 0;
  // writeStat2(stat2);

  // pretty_print(inputs.size(), bytes,
  //              bench([&hashes, &scountBloom, &basic_count]()
  //                    {
  //                for (int i = 0;i < data_size;i++) {
  //                  basic_count +=
  //                      scountBloom.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // printf("Benchmarking construction speed\n");

  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &scountBloom_test, &size]()
  //                    { scountBloom_test.AddAll(test_hashes, 0, size); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(input_volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = 1 - scountBloom.Contain(hashes[i]);
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

  // printf("-------------- Succinct Counting Blocked Bloom 10 Filter --------------\n");
  SuccinctCountingBloomFilter<uint64_t, 16, true> sCntBlBloom(filter_size), sCntBlBloom_test(filter_size);

  // Construction
  sCntBlBloom.AddAll(test_hashes, 0, test_size);

  // Let us check the size of the filter in bytes:
  filter_volume = sCntBlBloom.SizeInBytes();
  // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  //        100.0 * filter_volume / bytes);
  // printf("\nfilter memory usage : %1.f bits/entry\n",
  //        8.0 * filter_volume / test_hashes.size());
  // printf("\n");

  // Let us test the query with bogus strings
  fp_bogus = 0;
  for (int i = 0; i < bogus_size; i++)
  {
    bool in_set = 1 - sCntBlBloom.Contain(bogus_hashes[i]);
    if (in_set)
    {
      fp_bogus++;
    }
  }

  // printf("Bogus false-positives: %zu\n", fpp);
  // printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // printf("Benchmarking queries:\n");

  basic_count = 0;
  writeStat2(stat2);

  pretty_print(inputs.size(), bytes,
               bench([&hashes, &sCntBlBloom, &basic_count]()
                     {
                 for (int i = 0;i < data_size;i++) {
                   basic_count +=
                       sCntBlBloom.Contain(hashes[i]);
                 } }),
               stat2);

  // printf("Benchmarking construction speed\n");
  basic_count = 0;
  pretty_print(test_hashes.size(), bytes,
               bench([&test_hashes, &sCntBlBloom_test, &basic_count]()
                     {
               for(basic_count = 0; basic_count < test_size; basic_count++) {
                  sCntBlBloom_test.Add(test_hashes[basic_count]);
                } }),
               stat2);

  fprintf(stat2, "\n");

  writeStat1(input_volume, bytes, filter_volume, stat1);

  // Testing
  for (int i = 0; i < data_size; i++)
  {
    dataValidity[i].second = 1 - sCntBlBloom.Contain(hashes[i]);
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

  // printf("-------------- Succinct Counting Blocked Bloom Rank 10 Filter --------------\n");
  // SuccinctCountingBlockedBloomRankFilter<uint64_t, 10, SimpleMixSplit> scbrBloom(filter_size);
  BloomFilter<uint64_t, 8, true> bBloom_8(filter_size), bBloom_8_test(filter_size);

  // Construction
  bBloom_8.AddAll(test_hashes, 0, test_size);

  // Let us check the size of the filter in bytes:
  filter_volume = bBloom_8.SizeInBytes();
  // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  //        100.0 * filter_volume / bytes);
  // printf("\nfilter memory usage : %1.f bits/entry\n",
  //        8.0 * filter_volume / test_hashes.size());
  // printf("\n");

  // Let us test the query with bogus strings
  fp_bogus = 0;
  for (int i = 0; i < bogus_size; i++)
  {
    bool in_set = 1 - bBloom_8.Contain(bogus_hashes[i]);
    if (in_set)
    {
      fp_bogus++;
    }
  }

  // printf("Bogus false-positives: %zu\n", fpp);
  // printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // printf("Benchmarking queries:\n");

  basic_count = 0;
  writeStat2(stat2);

  pretty_print(inputs.size(), bytes,
               bench([&hashes, &bBloom_8, &basic_count]()
                     {
                 for (int i = 0;i < data_size;i++) {
                   basic_count +=
                       bBloom_8.Contain(hashes[i]);
                 } }),
               stat2);

  // printf("Benchmarking construction speed\n");

  pretty_print(test_hashes.size(), bytes,
               bench([&test_hashes, &bBloom_8_test, &size]()
                     { bBloom_8_test.AddAll(test_hashes, 0, size); }),
               stat2);

  fprintf(stat2, "\n");

  writeStat1(input_volume, bytes, filter_volume, stat1);

  // Testing
  for (int i = 0; i < data_size; i++)
  {
    dataValidity[i].second = 1 - bBloom_8.Contain(hashes[i]);
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

  return EXIT_SUCCESS;
}