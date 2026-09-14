#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <thread>

#include "CycleTimer.h"

using namespace std;


typedef struct {
  // Control work assignments
  int start, end;

  // Shared by all functions
  double *data;
  double *clusterCentroids;
  int *clusterAssignments;
  double *currCost;
  int M, N, K;
} WorkerArgs;


/**
 * Checks if the algorithm has converged.
 *
 * NOTE: DO NOT MODIFY THIS FUNCTION!!!
 */
static bool stoppingConditionMet(double *prevCost,
                                 double *currCost,
                                 double epsilon,
                                 int K) {
  for (int k = 0; k < K; k++) {
    if (abs(prevCost[k] - currCost[k]) > epsilon)
      return false;
  }

  return true;
}


/**
 * Computes L2 distance between two points of dimension nDim.
 */
double dist(double *x, double *y, int nDim) {
  double accum = 0.0;

  for (int i = 0; i < nDim; i++) {
    accum += pow((x[i] - y[i]), 2);
  }

  return sqrt(accum);
}


/**
 * Assigns each data point to its closest cluster centroid.
 *
 * Each invocation works only on the data-point range:
 *
 *     [args->start, args->end)
 *
 * This allows different threads to process different data points
 * without writing to the same clusterAssignments entries.
 */
void computeAssignments(WorkerArgs *const args) {

  for (int m = args->start; m < args->end; m++) {

    double minDistSquared = 1e30;
    int bestAssignment = -1;

    double *point = &args->data[m * args->N];

    for (int k = 0; k < args->K; k++) {

      double *centroid =
          &args->clusterCentroids[k * args->N];

      double distSquared = 0.0;

      for (int n = 0; n < args->N; n++) {
        double diff = point[n] - centroid[n];
        distSquared += diff * diff;
      }

      if (distSquared < minDistSquared) {
        minDistSquared = distSquared;
        bestAssignment = k;
      }
    }

    args->clusterAssignments[m] = bestAssignment;
  }
}


/**
 * Given the cluster assignments, computes the new centroid locations
 * for each cluster.
 */
void computeCentroids(WorkerArgs *const args) {

  int *counts = new int[args->K];

  // Zero things out
  for (int k = 0; k < args->K; k++) {

    counts[k] = 0;

    for (int n = 0; n < args->N; n++) {
      args->clusterCentroids[k * args->N + n] = 0.0;
    }
  }

  // Sum contributions from assigned examples
  for (int m = 0; m < args->M; m++) {

    int k = args->clusterAssignments[m];

    for (int n = 0; n < args->N; n++) {
      args->clusterCentroids[k * args->N + n] +=
          args->data[m * args->N + n];
    }

    counts[k]++;
  }

  // Compute means
  for (int k = 0; k < args->K; k++) {

    counts[k] = max(counts[k], 1);

    for (int n = 0; n < args->N; n++) {
      args->clusterCentroids[k * args->N + n] /= counts[k];
    }
  }

  delete[] counts;
}


/**
 * Computes the per-cluster cost.
 * Used to check whether the algorithm has converged.
 */
void computeCost(WorkerArgs *const args) {

  double *accum = new double[args->K];

  // Zero things out
  for (int k = 0; k < args->K; k++) {
    accum[k] = 0.0;
  }

  // Sum cost for all data points assigned to centroid
  for (int m = 0; m < args->M; m++) {

    int k = args->clusterAssignments[m];

    accum[k] += dist(
        &args->data[m * args->N],
        &args->clusterCentroids[k * args->N],
        args->N
    );
  }

  // Update costs
  for (int k = args->start; k < args->end; k++) {
    args->currCost[k] = accum[k];
  }

  delete[] accum;
}


/**
 * Computes the K-Means algorithm using std::thread.
 *
 * Only computeAssignments() is parallelized.
 */
void kMeansThread(double *data,
                  double *clusterCentroids,
                  int *clusterAssignments,
                  int M,
                  int N,
                  int K,
                  double epsilon) {

  // Used to track convergence
  double *prevCost = new double[K];
  double *currCost = new double[K];


  // Shared arguments
  WorkerArgs args;

  args.data = data;
  args.clusterCentroids = clusterCentroids;
  args.clusterAssignments = clusterAssignments;
  args.currCost = currCost;

  args.M = M;
  args.N = N;
  args.K = K;


  // Your machine has 4 hardware threads
  const int numThreads = 4;

  std::thread workers[numThreads];
  WorkerArgs threadArgs[numThreads];


  // Initialize cost arrays
  for (int k = 0; k < K; k++) {
    prevCost[k] = 1e30;
    currCost[k] = 0.0;
  }


  /* Main K-Means Algorithm Loop */

  int iter = 0;

  while (!stoppingConditionMet(
      prevCost,
      currCost,
      epsilon,
      K)) {

    // Save previous costs
    for (int k = 0; k < K; k++) {
      prevCost[k] = currCost[k];
    }


    /*
     * -----------------------------------------
     * Parallel computeAssignments()
     * -----------------------------------------
     *
     * Divide M data points into 4 chunks.
     */

    int chunkSize =
        (M + numThreads - 1) / numThreads;


    // Prepare arguments for each worker
    for (int t = 0; t < numThreads; t++) {

      threadArgs[t] = args;

      threadArgs[t].start =
          t * chunkSize;

      threadArgs[t].end =
          std::min(
              M,
              (t + 1) * chunkSize
          );
    }


    /*
     * Spawn threads 1, 2 and 3.
     *
     * The main thread itself handles chunk 0,
     * giving four workers in total.
     */

    for (int t = 1; t < numThreads; t++) {

      workers[t] = std::thread(
          computeAssignments,
          &threadArgs[t]
      );
    }


    // Main thread performs chunk 0
    computeAssignments(&threadArgs[0]);


    // Wait for the other workers
    for (int t = 1; t < numThreads; t++) {
      workers[t].join();
    }


    /*
     * -----------------------------------------
     * Serial portions
     * -----------------------------------------
     */

    args.start = 0;
    args.end = K;

    computeCentroids(&args);
    computeCost(&args);


    iter++;
  }


  delete[] currCost;
  delete[] prevCost;
}
