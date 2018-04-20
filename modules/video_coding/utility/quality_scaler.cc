/*
 *  Copyright (c) 2014 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/video_coding/utility/quality_scaler.h"

#include <math.h>

#include <algorithm>
#include <memory>

#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/task_queue.h"

// TODO(kthelgason): Some versions of Android have issues with log2.
// See https://code.google.com/p/android/issues/detail?id=212634 for details
#if defined(WEBRTC_ANDROID)
#define log2(x) (log(x) / log(2))
#endif

namespace webrtc {

namespace {
// TODO(nisse): Delete, delegate to encoders.
// Threshold constant used until first downscale (to permit fast rampup).
static const int kMeasureMs = 2000;
static const float kSamplePeriodScaleFactor = 2.5;
static const int kFramedropPercentThreshold = 60;
static const int kMinFramesNeededToScale = 2 * 30;

}  // namespace

class QualityScaler::CheckQpTask : public rtc::QueuedTask {
 public:
  explicit CheckQpTask(QualityScaler* scaler) : scaler_(scaler) {
    RTC_LOG(LS_INFO) << "Created CheckQpTask. Scheduling on queue...";
    rtc::TaskQueue::Current()->PostDelayedTask(
        std::unique_ptr<rtc::QueuedTask>(this), scaler_->GetSamplingPeriodMs());
  }
  void Stop() {
    RTC_DCHECK_CALLED_SEQUENTIALLY(&task_checker_);
    RTC_LOG(LS_INFO) << "Stopping QP Check task.";
    stop_ = true;
  }

 private:
  bool Run() override {
    RTC_DCHECK_CALLED_SEQUENTIALLY(&task_checker_);
    if (stop_)
      return true;  // TaskQueue will free this task.
    scaler_->CheckQp();
    rtc::TaskQueue::Current()->PostDelayedTask(
        std::unique_ptr<rtc::QueuedTask>(this), scaler_->GetSamplingPeriodMs());
    return false;  // Retain the task in order to reuse it.
  }

  QualityScaler* const scaler_;
  bool stop_ = false;
  rtc::SequencedTaskChecker task_checker_;
};

QualityScaler::QualityScaler(AdaptationObserverInterface* observer,
                             VideoEncoder::QpThresholds thresholds)
    : QualityScaler(observer, thresholds, kMeasureMs) {}

// Protected ctor, should not be called directly.
QualityScaler::QualityScaler(AdaptationObserverInterface* observer,
                             VideoEncoder::QpThresholds thresholds,
                             int64_t sampling_period_ms)
    : check_qp_task_(nullptr),
      observer_(observer),
      thresholds_(thresholds),
      sampling_period_ms_(sampling_period_ms),
      fast_rampup_(true),
      // Arbitrarily choose size based on 30 fps for 5 seconds.
      average_qp_(5 * 30),
      framedrop_percent_(5 * 30) {
  RTC_DCHECK_CALLED_SEQUENTIALLY(&task_checker_);
  RTC_DCHECK(observer_ != nullptr);
  check_qp_task_ = new CheckQpTask(this);
  RTC_LOG(LS_INFO) << "QP thresholds: low: " << thresholds_.low
                   << ", high: " << thresholds_.high;
}

QualityScaler::~QualityScaler() {
  RTC_DCHECK_CALLED_SEQUENTIALLY(&task_checker_);
  check_qp_task_->Stop();
}

int64_t QualityScaler::GetSamplingPeriodMs() const {
  RTC_DCHECK_CALLED_SEQUENTIALLY(&task_checker_);
  return fast_rampup_ ? sampling_period_ms_
                      : (sampling_period_ms_ * kSamplePeriodScaleFactor);
}

void QualityScaler::ReportDroppedFrame() {
  RTC_DCHECK_CALLED_SEQUENTIALLY(&task_checker_);
  framedrop_percent_.AddSample(100);
}

void QualityScaler::ReportQp(int qp) {
  RTC_DCHECK_CALLED_SEQUENTIALLY(&task_checker_);
  framedrop_percent_.AddSample(0);
  average_qp_.AddSample(qp);
}

void QualityScaler::CheckQp() {
  RTC_DCHECK_CALLED_SEQUENTIALLY(&task_checker_);
  // Should be set through InitEncode -> Should be set by now.
  RTC_DCHECK_GE(thresholds_.low, 0);

  // If we have not observed at least this many frames we can't make a good
  // scaling decision.
  if (framedrop_percent_.size() < kMinFramesNeededToScale)
    return;

  // Check if we should scale down due to high frame drop.
  const rtc::Optional<int> drop_rate = framedrop_percent_.GetAverage();
  if (drop_rate && *drop_rate >= kFramedropPercentThreshold) {
    RTC_LOG(LS_INFO) << "Reporting high QP, framedrop percent " << *drop_rate;
    ReportQpHigh();
    return;
  }

  // Check if we should scale up or down based on QP.
  const rtc::Optional<int> avg_qp = average_qp_.GetAverage();
  if (avg_qp) {
    RTC_LOG(LS_INFO) << "Checking average QP " << *avg_qp;
    if (*avg_qp > thresholds_.high) {
      ReportQpHigh();
      return;
    }
    if (*avg_qp <= thresholds_.low) {
      // QP has been low. We want to try a higher resolution.
      ReportQpLow();
      return;
    }
  }
}

void QualityScaler::ReportQpLow() {
  RTC_DCHECK_CALLED_SEQUENTIALLY(&task_checker_);
  ClearSamples();
  observer_->AdaptUp(AdaptationObserverInterface::AdaptReason::kQuality);
}

void QualityScaler::ReportQpHigh() {
  RTC_DCHECK_CALLED_SEQUENTIALLY(&task_checker_);
  ClearSamples();
  observer_->AdaptDown(AdaptationObserverInterface::AdaptReason::kQuality);
  // If we've scaled down, wait longer before scaling up again.
  if (fast_rampup_) {
    fast_rampup_ = false;
  }
}

void QualityScaler::ClearSamples() {
  RTC_DCHECK_CALLED_SEQUENTIALLY(&task_checker_);
  framedrop_percent_.Reset();
  average_qp_.Reset();
}
}  // namespace webrtc
