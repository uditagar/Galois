#include "PageRankMain.h"

namespace pagerank {


namespace cll = llvm::cl;
static cll::opt<std::string> transposeFile("transpose", cll::desc("<transpose file>"), cll::Required);



typedef typename Galois::Graph::LC_CSR_Graph<PNode,void>
  ::template with_numa_alloc<true>::type
  ::template with_no_lockable<true>::type
  InnerGraph;

typedef Galois::Graph::LC_InOut_Graph<InnerGraph> Graph;
typedef Graph::GraphNode GNode;

class PageRankAlamere: public PageRankMain<Graph> {

public:

  static const bool DOALL_STEAL = true;
  static const bool DEBUG = false;

  typedef std::vector<GNode> VecGNode;

  virtual std::string getVersion(void) {
    return "Pull-Topo-Both-Graphs";
  }
  void initGraph(Graph& graph) {

    Galois::Graph::readGraph(graph, inputFile, transposeFile);

    // size_t numEdges = 0; 
    // size_t selfEdges = 0;
    ParCounter numEdges;
    ParCounter selfEdges;

    Galois::StatTimer t_init("Time for initializing PageRank data: ");

    t_init.start();

    Galois::do_all_choice(Galois::Runtime::makeLocalRange(graph),
        [&] (GNode n) {
              unsigned out_deg = 0;
              for (auto j = graph.edge_begin(n, Galois::MethodFlag::UNPROTECTED)
                , endj = graph.edge_end(n, Galois::MethodFlag::UNPROTECTED); j != endj; ++j) {
                GNode neigh = graph.getEdgeDst(j);
                if (n != neigh) {
                  out_deg += 1;
                } else{
                  selfEdges += 1;
                }
              }


              if (DEBUG) {
                int in_deg = std::distance(graph.in_edge_begin(n, Galois::MethodFlag::UNPROTECTED)
                , graph.in_edge_end(n, Galois::MethodFlag::UNPROTECTED));
                std::cout << "Node: " << graph.idFromNode(n) << " has out degree: " << out_deg 
                  << ", in degree: " << in_deg << std::endl;
              }

              graph.getData(n, Galois::MethodFlag::UNPROTECTED) = PNode(double(initVal), out_deg);

              numEdges += out_deg;

            },
        std::make_tuple(
            Galois::loopname("init_loop"), 
            Galois::chunk_size<DEFAULT_CHUNK_SIZE>()));

    t_init.stop();

    std::cout << "Graph read with: " << graph.size() << " nodes, and: " << numEdges.reduce()
      << " non-self edges" << std::endl;
    std::cout << "Number of selfEdges: " << selfEdges.reduce() << std::endl;
    
  }

  struct PageRankOp {

    Graph& graph;
    const unsigned round;
    Galois::GReduceLogicalAND& allConverged; 
    
    PageRankOp(
        Graph& graph,
        const unsigned round,
        Galois::GReduceLogicalAND& allConverged)
      : 
        graph(graph),
        round(round),
        allConverged(allConverged)
    {}

    template <typename C>
    void operator () (GNode src, C&) {
      (*this)(src);
    }

    void operator () (GNode src) {


      double sumRanks = 0.0;

      if (DEBUG) {
        std::cout << "Processing Node: " << graph.idFromNode(src) << std::endl;
      }

      for (auto ni = graph.in_edge_begin(src, Galois::MethodFlag::UNPROTECTED)
          , endni = graph.in_edge_end(src, Galois::MethodFlag::UNPROTECTED); ni != endni; ++ni) {

        GNode pred = graph.getInEdgeDst(ni);

        if (pred != src) { // non-self edge
          const PNode& predData = graph.getData(pred, Galois::MethodFlag::UNPROTECTED);
          sumRanks += predData.getScaledValue(round);

          if (DEBUG) {
            std::cout << "Value from Neighbor: " << graph.idFromNode(pred) 
              << " is: " << predData.getScaledValue(round) << std::endl;
          }

        }
      }

      PNode& srcData = graph.getData(src, Galois::MethodFlag::UNPROTECTED);

      double updatedValue = randJmp + (1 - randJmp) * sumRanks;

      double currValue = srcData.getValue(round);

      srcData.setValue(round, updatedValue);

      if (std::fabs(currValue - updatedValue) > double(tolerance)) {
        allConverged.update(false);
      }

      // if (round > 100) {
        // std::cout << "Difference: " << (currValue - updatedValue) << std::endl;
      // }
      
    }
  };

  virtual size_t runPageRank(Graph& graph) {

    unsigned round = 0;

    while (true) {

      Galois::GReduceLogicalAND allConverged;

      Galois::do_all_choice(Galois::Runtime::makeLocalRange(graph),
          PageRankOp(graph, round, allConverged), 
          std::make_tuple(
            Galois::loopname("page_rank_inner"), 
            Galois::chunk_size<DEFAULT_CHUNK_SIZE>()));



      if (DEBUG) {
        std::cout << "Finished round: " << round << std::endl;

        for (auto i = graph.begin(), endi = graph.end();
            i != endi; ++i) {

          PNode& data = graph.getData(*i, Galois::MethodFlag::UNPROTECTED);

          std::cout << "Node: " << graph.idFromNode(*i) << ", page ranke values: " << 
            data.getValue(round) << ", " << data.getValue(round + 1) << std::endl;
        }
      }


      if (allConverged.reduceRO()) {
        break;
      }

      ++round;


    }

    std::cout << "number of rounds completed: " << round << std::endl;

    return round * graph.size();

  }
};

} // end namespace pagerank

int main (int argc, char* argv[]) {
  pagerank::PageRankAlamere p;
  p.run(argc, argv);
  return 0;
}
