/*!
 *  Copyright (c) 2015 by Contributors
 * \file iter_image_recordio-inl.hpp
 * \brief recordio data iterator
 */
#include <mxnet/io.h>
#include <dmlc/base.h>
#include <dmlc/io.h>
#include <dmlc/omp.h>
#include <dmlc/logging.h>
#include <dmlc/parameter.h>
#include <dmlc/recordio.h>
#include <dmlc/threadediter.h>
#include <unordered_map>
#include <vector>
#include <cstdlib>
#include "./inst_vector.h"
#include "./image_recordio.h"
#include "./image_augmenter.h"
#include "./iter_prefetcher.h"
#include "./iter_normalize.h"
#include "./iter_batchloader.h"

namespace mxnet {
namespace io {
/*! \brief data structure to hold labels for images */
class ImageLabelMap {
 public:
  /*!
   * \brief initialize the label list into memory
   * \param path_imglist path to the image list
   * \param label_width predefined label_width
   */
  explicit ImageLabelMap(const char *path_imglist,
                         mshadow::index_t label_width,
                         bool silent) {
    this->label_width = label_width;
    image_index_.clear();
    label_.clear();
    idx2label_.clear();
    dmlc::InputSplit *fi = dmlc::InputSplit::Create
        (path_imglist, 0, 1, "text");
    dmlc::InputSplit::Blob rec;
    while (fi->NextRecord(&rec)) {
      // quick manual parsing
      char *p = reinterpret_cast<char*>(rec.dptr);
      char *end = p + rec.size;
      // skip space
      while (isspace(*p) && p != end) ++p;
      image_index_.push_back(static_cast<size_t>(atol(p)));
      for (size_t i = 0; i < label_width; ++i) {
        // skip till space
        while (!isspace(*p) && p != end) ++p;
        // skip space
        while (isspace(*p) && p != end) ++p;
        CHECK(p != end) << "Bad ImageList format";
        label_.push_back(static_cast<real_t>(atof(p)));
      }
    }
    delete fi;
    // be careful not to resize label_ afterwards
    idx2label_.reserve(image_index_.size());
    for (size_t i = 0; i < image_index_.size(); ++i) {
      idx2label_[image_index_[i]] = dmlc::BeginPtr(label_) + i * label_width;
    }
    if (!silent) {
      LOG(INFO) << "Loaded ImageList from " << path_imglist << ' '
                << image_index_.size() << " Image records";
    }
  }
  /*! \brief find a label for corresponding index */
  inline mshadow::Tensor<cpu, 1> Find(size_t imid) const {
    std::unordered_map<size_t, real_t*>::const_iterator it
        = idx2label_.find(imid);
    CHECK(it != idx2label_.end()) << "fail to find imagelabel for id " << imid;
    return mshadow::Tensor<cpu, 1>(it->second, mshadow::Shape1(label_width));
  }

 private:
  // label with_
  mshadow::index_t label_width;
  // image index of each record
  std::vector<size_t> image_index_;
  // real label content
  std::vector<real_t> label_;
  // map index to label
  std::unordered_map<size_t, real_t*> idx2label_;
};

// Define image record parser parameters
struct ImageRecParserParam : public dmlc::Parameter<ImageRecParserParam> {
  /*! \brief path to image list */
  std::string path_imglist;
  /*! \brief path to image recordio */
  std::string path_imgrec;
  /*! \brief label-width */
  int label_width;
  /*! \brief input shape */
  TShape data_shape;
  /*! \brief number of threads */
  int preprocess_threads;
  /*! \brief whether to remain silent */
  bool verbose;
  /*! \brief partition the data into multiple parts */
  int num_parts;
  /*! \brief the index of the part will read*/
  int part_index;

  // declare parameters
  DMLC_DECLARE_PARAMETER(ImageRecParserParam) {
    DMLC_DECLARE_FIELD(path_imglist).set_default("")
        .describe("Dataset Param: Path to image list.");
    DMLC_DECLARE_FIELD(path_imgrec).set_default("./data/imgrec.rec")
        .describe("Dataset Param: Path to image record file.");
    DMLC_DECLARE_FIELD(label_width).set_lower_bound(1).set_default(1)
        .describe("Dataset Param: How many labels for an image.");
    DMLC_DECLARE_FIELD(data_shape)
        .enforce_nonzero()
        .describe("Dataset Param: Shape of each instance generated by the DataIter.");
    DMLC_DECLARE_FIELD(preprocess_threads).set_lower_bound(1).set_default(4)
        .describe("Backend Param: Number of thread to do preprocessing.");
    DMLC_DECLARE_FIELD(verbose).set_default(true)
        .describe("Auxiliary Param: Whether to output parser information.");
    DMLC_DECLARE_FIELD(num_parts).set_default(1)
        .describe("partition the data into multiple parts");
    DMLC_DECLARE_FIELD(part_index).set_default(0)
        .describe("the index of the part will read");
  }
};

// parser to parse image recordio
class ImageRecordIOParser {
 public:
  ImageRecordIOParser(void)
      : source_(nullptr),
        label_map_(nullptr) {
  }
  ~ImageRecordIOParser(void) {
    // can be nullptr
    delete label_map_;
    delete source_;
    for (size_t i = 0; i < augmenters_.size(); ++i) {
      delete augmenters_[i];
    }
    for (size_t i = 0; i < prnds_.size(); ++i) {
      delete prnds_[i];
    }
  }
  // initialize the parser
  inline void Init(const std::vector<std::pair<std::string, std::string> >& kwargs);

  // set record to the head
  inline void BeforeFirst(void) {
    return source_->BeforeFirst();
  }
  // parse next set of records, return an array of
  // instance vector to the user
  inline bool ParseNext(std::vector<InstVector> *out);

 private:
  // magic nyumber to see prng
  static const int kRandMagic = 111;
  /*! \brief parameters */
  ImageRecParserParam param_;
  /*! \brief augmenters */
  std::vector<ImageAugmenter*> augmenters_;
  /*! \brief random samplers */
  std::vector<common::RANDOM_ENGINE*> prnds_;
  /*! \brief data source */
  dmlc::InputSplit *source_;
  /*! \brief label information, if any */
  ImageLabelMap *label_map_;
  /*! \brief temp space */
  mshadow::TensorContainer<cpu, 3> img_;
};

inline void ImageRecordIOParser::Init(
        const std::vector<std::pair<std::string, std::string> >& kwargs) {
#if MXNET_USE_OPENCV
  // initialize parameter
  // init image rec param
  param_.InitAllowUnknown(kwargs);
  int maxthread, threadget;
  #pragma omp parallel
  {
    // be conservative, set number of real cores
    maxthread = std::max(omp_get_num_procs() / 2 - 1, 1);
  }
  param_.preprocess_threads = std::min(maxthread, param_.preprocess_threads);
  #pragma omp parallel num_threads(param_.preprocess_threads)
  {
    threadget = omp_get_num_threads();
  }
  param_.preprocess_threads = threadget;

  // setup decoders
  for (int i = 0; i < threadget; ++i) {
    augmenters_.push_back(new ImageAugmenter());
    augmenters_[i]->Init(kwargs);
    prnds_.push_back(new common::RANDOM_ENGINE((i + 1) * kRandMagic));
  }
  if (param_.path_imglist.length() != 0) {
    label_map_ = new ImageLabelMap(param_.path_imglist.c_str(),
                                   param_.label_width, !param_.verbose);
  } else {
    param_.label_width = 1;
  }
  CHECK(param_.path_imgrec.length() != 0)
      << "ImageRecordIOIterator: must specify image_rec";

  if (param_.verbose) {
    LOG(INFO) << "ImageRecordIOParser: " << param_.path_imgrec
              << ", use " << threadget << " threads for decoding..";
  }
  source_ = dmlc::InputSplit::Create(
      param_.path_imgrec.c_str(), param_.part_index,
      param_.num_parts, "recordio");
  // use 64 MB chunk when possible
  source_->HintChunkSize(8 << 20UL);
#else
  LOG(FATAL) << "ImageRec need opencv to process";
#endif
}

inline bool ImageRecordIOParser::
ParseNext(std::vector<InstVector> *out_vec) {
  CHECK(source_ != nullptr);
  dmlc::InputSplit::Blob chunk;
  if (!source_->NextChunk(&chunk)) return false;
#if MXNET_USE_OPENCV
  // save opencv out
  out_vec->resize(param_.preprocess_threads);
  #pragma omp parallel num_threads(param_.preprocess_threads)
  {
    CHECK(omp_get_num_threads() == param_.preprocess_threads);
    int tid = omp_get_thread_num();
    dmlc::RecordIOChunkReader reader(chunk, tid, param_.preprocess_threads);
    ImageRecordIO rec;
    dmlc::InputSplit::Blob blob;
    // image data
    InstVector &out = (*out_vec)[tid];
    out.Clear();
    while (reader.NextRecord(&blob)) {
      // Opencv decode and augments
      cv::Mat res;
      rec.Load(blob.dptr, blob.size);
      cv::Mat buf(1, rec.content_size, CV_8U, rec.content);
      // -1 to keep the number of channel of the encoded image, and not force gray or color.
      res = cv::imdecode(buf, -1);
      int n_channels = res.channels();
      res = augmenters_[tid]->Process(res, prnds_[tid]);
      out.Push(static_cast<unsigned>(rec.image_index()),
               mshadow::Shape3(n_channels, res.rows, res.cols),
               mshadow::Shape1(param_.label_width));

      mshadow::Tensor<cpu, 3> data = out.data().Back();
      // Substract mean value on each channel.
      if (n_channels == 3) {
        for (int i = 0; i < res.rows; ++i) {
          for (int j = 0; j < res.cols; ++j) {
            cv::Vec3b bgr = res.at<cv::Vec3b>(i, j);
            data[0][i][j] = bgr[2];
            data[1][i][j] = bgr[1];
            data[2][i][j] = bgr[0];
          }
        }
      } else {
        for (int i = 0; i < res.rows; ++i) {
          for (int j = 0; j < res.cols; ++j) {
            data[0][i][j] = res.at<uint8_t>(i, j);
          }
        }
      }
      // If this is a grayscale image, substract only mean_r.
      else {
        for (int i = 0; i < res.rows; ++i) {
          for (int j = 0; j < res.cols; ++j) { 
            data[0][i][j] = res.at<uint8_t>(i, j);
          }
        }
      }
      mshadow::Tensor<cpu, 1> label = out.label().Back();
      if (label_map_ != nullptr) {
        mshadow::Copy(label, label_map_->Find(rec.image_index()));
      } else {
        label[0] = rec.header.label;
      }
      res.release();
    }
  }
#else
      LOG(FATAL) << "Opencv is needed for image decoding and augmenting.";
#endif
  return true;
}

// Define image record parameters
struct ImageRecordParam: public dmlc::Parameter<ImageRecordParam> {
  /*! \brief whether to do shuffle */
  bool shuffle;
  /*! \brief random seed */
  int seed;
  /*! \brief whether to remain silent */
  bool verbose;
  // declare parameters
  DMLC_DECLARE_PARAMETER(ImageRecordParam) {
    DMLC_DECLARE_FIELD(shuffle).set_default(false)
        .describe("Augmentation Param: Whether to shuffle data.");
    DMLC_DECLARE_FIELD(seed).set_default(0)
        .describe("Augmentation Param: Random Seed.");
    DMLC_DECLARE_FIELD(verbose).set_default(true)
        .describe("Auxiliary Param: Whether to output information.");
  }
};

// iterator on image recordio
class ImageRecordIter : public IIterator<DataInst> {
 public:
  ImageRecordIter() : data_(nullptr) { }
  // destructor
  virtual ~ImageRecordIter(void) {
    iter_.Destroy();
    delete data_;
  }
  // constructor
  virtual void Init(const std::vector<std::pair<std::string, std::string> >& kwargs) {
    param_.InitAllowUnknown(kwargs);
    // use the kwarg to init parser
    parser_.Init(kwargs);
    // prefetch at most 4 minbatches
    iter_.set_max_capacity(4);
    // init thread iter
    iter_.Init([this](std::vector<InstVector> **dptr) {
        if (*dptr == nullptr) {
          *dptr = new std::vector<InstVector>();
        }
        return parser_.ParseNext(*dptr);
      },
      [this]() { parser_.BeforeFirst(); });
    inst_ptr_ = 0;
    rnd_.seed(kRandMagic + param_.seed);
  }
  // before first
  virtual void BeforeFirst(void) {
    iter_.BeforeFirst();
    inst_order_.clear();
    inst_ptr_ = 0;
  }

  virtual bool Next(void) {
    while (true) {
      if (inst_ptr_ < inst_order_.size()) {
        std::pair<unsigned, unsigned> p = inst_order_[inst_ptr_];
        out_ = (*data_)[p.first][p.second];
        ++inst_ptr_;
        return true;
      } else {
        if (data_ != nullptr) iter_.Recycle(&data_);
        if (!iter_.Next(&data_)) return false;
        inst_order_.clear();
        for (unsigned i = 0; i < data_->size(); ++i) {
          const InstVector& tmp = (*data_)[i];
          for (unsigned j = 0; j < tmp.Size(); ++j) {
            inst_order_.push_back(std::make_pair(i, j));
          }
        }
        // shuffle instance order if needed
        if (param_.shuffle != 0) {
          std::shuffle(inst_order_.begin(), inst_order_.end(), rnd_);
        }
        inst_ptr_ = 0;
      }
    }
    return false;
  }

  virtual const DataInst &Value(void) const {
    return out_;
  }

 private:
  // random magic
  static const int kRandMagic = 111;
  // output instance
  DataInst out_;
  // data ptr
  size_t inst_ptr_;
  // internal instance order
  std::vector<std::pair<unsigned, unsigned> > inst_order_;
  // data
  std::vector<InstVector> *data_;
  // internal parser
  ImageRecordIOParser parser_;
  // backend thread
  dmlc::ThreadedIter<std::vector<InstVector> > iter_;
  // parameters
  ImageRecordParam param_;
  // random number generator
  common::RANDOM_ENGINE rnd_;
};

DMLC_REGISTER_PARAMETER(ImageRecParserParam);
DMLC_REGISTER_PARAMETER(ImageRecordParam);

MXNET_REGISTER_IO_ITER(ImageRecordIter)
.describe("Create iterator for dataset packed in recordio.")
.add_arguments(ImageRecParserParam::__FIELDS__())
.add_arguments(ImageRecordParam::__FIELDS__())
.add_arguments(BatchParam::__FIELDS__())
.add_arguments(PrefetcherParam::__FIELDS__())
.add_arguments(ImageAugmentParam::__FIELDS__())
.add_arguments(ImageNormalizeParam::__FIELDS__())
.set_body([]() {
    return new PrefetcherIter(
        new BatchLoader(
            new ImageNormalizeIter(
                new ImageRecordIter())));
  });
}  // namespace io
}  // namespace mxnet
