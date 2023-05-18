#include "transcoder_filter.h"

#include "filter/filter_resampler.h"
#include "filter/filter_rescaler.h"
#include "transcoder_gpu.h"
#include "transcoder_private.h"

using namespace cmn;

#define PTS_INCREMENT_LIMIT 15

TranscodeFilter::TranscodeFilter() : _impl(nullptr)
{
}

TranscodeFilter::~TranscodeFilter()
{
}

bool TranscodeFilter::Configure(int32_t filter_id,
								const std::shared_ptr<info::Stream> &input_stream_info, std::shared_ptr<MediaTrack> input_track,
								const std::shared_ptr<info::Stream> &output_stream_info, std::shared_ptr<MediaTrack> output_track,
								CompleteHandler complete_handler)
{
	logtd("Create a transcode filter. InputTrack(%d). Type(%s)", input_track->GetId(), (input_track->GetMediaType() == MediaType::Video) ? "Video" : "Audio");

	_filter_id = filter_id;
	_input_stream_info = input_stream_info;
	_input_track = input_track;
	_output_stream_info = output_stream_info;
	_output_track = output_track;
	_complete_handler = complete_handler;
	_threshold_ts_increment = (int64_t)_input_track->GetTimeBase().GetTimescale() * PTS_INCREMENT_LIMIT;

	return CreateFilter();
}

bool TranscodeFilter::CreateFilter()
{
	std::lock_guard<std::shared_mutex> lock(_mutex);

	if (_impl != nullptr)
	{
		_impl->Stop();
		_impl.reset();
		_impl = nullptr;
	}

	switch (_input_track->GetMediaType())
	{
		case MediaType::Audio:
			_impl = std::make_shared<FilterResampler>();
			break;
		case MediaType::Video:
			_impl = std::make_shared<FilterRescaler>();
			break;
		default:
			logte("Unsupported media type in filter");
			return false;
	}

	auto urn = info::ManagedQueue::URN(_input_stream_info->GetApplicationName(), _input_stream_info->GetName().CStr(), "trs", ov::String::FormatString("filter_%s", cmn::GetMediaTypeString(_input_track->GetMediaType()).LowerCaseString().CStr()));
	_impl->SetQueueUrn(urn.CStr());
	_impl->SetCompleteHandler(bind(&TranscodeFilter::OnComplete, this, std::placeholders::_1));

	bool success = _impl->Configure(_input_track, _output_track);
	if (success == false)
	{
		logte("Could not create filter");

		return false;
	}

	return _impl->Start();
}

void TranscodeFilter::Stop() {
	
	std::lock_guard<std::shared_mutex> lock(_mutex);
	
	if (_impl != nullptr)
	{
		_impl->Stop();
		_impl.reset();
		_impl = nullptr;
	}
}

bool TranscodeFilter::SendBuffer(std::shared_ptr<MediaFrame> buffer)
{
	if (IsNeedUpdate(buffer) == true)
	{
		if (CreateFilter() == false)
		{
			logte("Failed to regenerate filter");

			return false;
		}
	}

	std::shared_lock<std::shared_mutex> lock(_mutex);
	if(_impl == nullptr)
	{
		return false;
	}

	return (_impl->SendBuffer(std::move(buffer)) == 0) ? true : false;
}

bool TranscodeFilter::IsNeedUpdate(std::shared_ptr<MediaFrame> buffer)
{
	// In case of pts/dts jumps
	int64_t ts_increment = abs(buffer->GetPts() - _last_pts);
	int64_t tmp_last_pts = _last_pts;
	bool detect_abnormal_increase_pts = (_last_pts != -1LL && ts_increment > _threshold_ts_increment) ? true : false;

	_last_pts = buffer->GetPts();

	if (detect_abnormal_increase_pts)
	{
		logtw("Timestamp has changed abnormally.  %lld -> %lld", tmp_last_pts, buffer->GetPts());

		return true;
	}

	// In case of resolution change
	std::shared_lock<std::shared_mutex> lock(_mutex);
	if (_impl == nullptr)
	{
		return false;
	}

	if (_input_track->GetMediaType() == MediaType::Video)
	{
		if (buffer->GetWidth() != (int32_t)_impl->GetInputWidth() || buffer->GetHeight() != (int32_t)_impl->GetInputHeight())
		{
			logti("Changed input resolution of %u track. (%dx%d -> %dx%d)", _input_track->GetId(), _impl->GetInputWidth(), _impl->GetInputHeight(), buffer->GetWidth(), buffer->GetHeight());
			_input_track->SetWidth(buffer->GetWidth());
			_input_track->SetHeight(buffer->GetHeight());
			return true;
		}
	}

	return false;
}

void TranscodeFilter::OnComplete(std::shared_ptr<MediaFrame> frame) {
	if(_complete_handler)
	{
		_complete_handler(_filter_id, frame);
	}
}

cmn::Timebase TranscodeFilter::GetInputTimebase() const
{
	return _impl->GetInputTimebase();
}

cmn::Timebase TranscodeFilter::GetOutputTimebase() const
{
	return _impl->GetOutputTimebase();
}

