#include "..\..\stdafx.h"

#include "input.h"

#include "../../video_format.h"

#include <common/concurrency/executor.h>
#include <common/diagnostics/graph.h>
#include <common/utility/printer.h>

#include <tbb/concurrent_queue.h>
#include <tbb/queuing_mutex.h>

#include <boost/exception/error_info.hpp>
#include <boost/thread/once.hpp>
#include <boost/thread/thread.hpp>

#include <errno.h>
#include <system_error>
		
#if defined(_MSC_VER)
#pragma warning (disable : 4244)
#endif

extern "C" 
{
	#define __STDC_CONSTANT_MACROS
	#define __STDC_LIMIT_MACROS
	#include <libavformat/avformat.h>
}

namespace caspar { namespace core { namespace ffmpeg{
		
struct input::implementation : boost::noncopyable
{		
	static const size_t PACKET_BUFFER_COUNT = 50;

	printer parent_printer_;
	safe_ptr<diagnostics::graph> graph_;

	std::shared_ptr<AVFormatContext> format_context_;	// Destroy this last

	std::shared_ptr<AVCodecContext>	video_codec_context_;
	std::shared_ptr<AVCodecContext>	audio_codex_context_;
	
	const std::wstring filename_;

	bool loop_;
	int video_s_index_;
	int	audio_s_index_;
		
	tbb::concurrent_bounded_queue<std::shared_ptr<aligned_buffer>> video_packet_buffer_;
	tbb::concurrent_bounded_queue<std::shared_ptr<aligned_buffer>> audio_packet_buffer_;

	boost::condition_variable cond_;
	boost::mutex mutex_;
	
	executor executor_;
public:
	explicit implementation(const safe_ptr<diagnostics::graph>& graph, const std::wstring& filename, bool loop, const printer& parent_printer) 
		: parent_printer_(parent_printer)
		, graph_(graph)
		, loop_(loop)
		, video_s_index_(-1)
		, audio_s_index_(-1)
		, filename_(filename)
		, executor_(print())
	{			
		graph_->set_color("input-buffer", diagnostics::color(1.0f, 1.0f, 0.0f));
		graph_->set_color("seek", diagnostics::color(0.5f, 1.0f, 0.5f));	

		int errn;
		AVFormatContext* weak_format_context_;
		if((errn = -av_open_input_file(&weak_format_context_, narrow(filename).c_str(), nullptr, 0, nullptr)) > 0)
			BOOST_THROW_EXCEPTION(
				file_read_error() << 
				source_info(narrow(print())) << 
				msg_info("No format context found.") << 
				boost::errinfo_api_function("av_open_input_file") <<
				boost::errinfo_errno(errn) <<
				boost::errinfo_file_name(narrow(filename)));

		format_context_.reset(weak_format_context_, av_close_input_file);
			
		if((errn = -av_find_stream_info(format_context_.get())) > 0)
			BOOST_THROW_EXCEPTION(
				file_read_error() << 
				source_info(narrow(print())) << 
				boost::errinfo_api_function("av_find_stream_info") <<
				msg_info("No stream found.") << 
				boost::errinfo_errno(errn));

		video_codec_context_ = open_stream(CODEC_TYPE_VIDEO, video_s_index_);
		if(!video_codec_context_)
			CASPAR_LOG(warning) << print() << " Could not open any video stream.";
		else
			fix_time_base(video_codec_context_.get());
		
		audio_codex_context_ = open_stream(CODEC_TYPE_AUDIO, audio_s_index_);
		if(!audio_codex_context_)
			CASPAR_LOG(warning) << print() << " Could not open any audio stream.";
		else
			fix_time_base(video_codec_context_.get());

		if(!video_codec_context_ && !audio_codex_context_)
			BOOST_THROW_EXCEPTION(
				file_read_error() << 
				source_info(narrow(print())) << 
				msg_info("No video or audio codec context found."));		
			
		executor_.start();
		executor_.begin_invoke([this]{read_file();});
		CASPAR_LOG(info) << print() << " Started.";
	}

	~implementation()
	{
		executor_.clear();
		executor_.stop();
		cond_.notify_all();
		CASPAR_LOG(info) << print() << " Stopped.";
	}
			
	void fix_time_base(AVCodecContext* context) // Some files give an invalid numerator, try to fix it.
	{
		if(context && context->time_base.num == 1)
			context->time_base.num = static_cast<int>(std::pow(10.0, static_cast<int>(std::log10(static_cast<float>(context->time_base.den)))-1));
	}

	std::shared_ptr<AVCodecContext> open_stream(int codec_type, int& s_index)
	{		
		AVStream** streams_end = format_context_->streams+format_context_->nb_streams;
		AVStream** stream = std::find_if(format_context_->streams, streams_end, 
			[&](AVStream* stream) { return stream != nullptr && stream->codec->codec_type == codec_type ;});
		
		if(stream == streams_end) 
			return nullptr;

		s_index = (*stream)->index;
		
		auto codec = avcodec_find_decoder((*stream)->codec->codec_id);			
		if(codec == nullptr)
			return nullptr;
			
		if((-avcodec_open((*stream)->codec, codec)) > 0)		
			return nullptr;

		return std::shared_ptr<AVCodecContext>((*stream)->codec, avcodec_close);
	}
		
	void read_file() // For every packet taken: read in a number of packets.
	{				
		AVPacket tmp_packet;
		safe_ptr<AVPacket> read_packet(&tmp_packet, av_free_packet);	

		if (av_read_frame(format_context_.get(), read_packet.get()) >= 0) // NOTE: read_packet is only valid until next call of av_safe_ptr<read_frame> or av_close_input_file
		{
			auto packet = std::make_shared<aligned_buffer>(read_packet->data, read_packet->data + read_packet->size);
			if(read_packet->stream_index == video_s_index_) 		
				video_packet_buffer_.try_push(std::move(packet));	
			else if(read_packet->stream_index == audio_s_index_) 	
				audio_packet_buffer_.try_push(std::move(packet));		
		}
		else if(!loop_ || !seek_frame(0, AVSEEK_FLAG_BACKWARD)) // TODO: av_seek_frame does not work for all formats
			executor_.stop();
		else
			graph_->tag("seek");

		boost::this_thread::yield();
			
		graph_->update("input-buffer", static_cast<float>(video_packet_buffer_.size())/static_cast<float>(PACKET_BUFFER_COUNT));		
		
		executor_.begin_invoke([this]{read_file();});		
		boost::unique_lock<boost::mutex> lock(mutex_);
		while(executor_.is_running() && audio_packet_buffer_.size() > PACKET_BUFFER_COUNT && video_packet_buffer_.size() > PACKET_BUFFER_COUNT)
			cond_.wait(lock);		
	}
	
	bool seek_frame(int64_t seek_target, int flags = 0)
	{  
		static const AVRational TIME_BASE_Q = {1, AV_TIME_BASE};
		
		int stream_index = std::max(video_s_index_, audio_s_index_);
		seek_target *= AV_TIME_BASE;

		if(stream_index >= 0)	  
			seek_target = av_rescale_q(seek_target, TIME_BASE_Q, format_context_->streams[stream_index]->time_base);
	  
		bool result = av_seek_frame(format_context_.get(), stream_index, seek_target, flags) >= 0;
		if(!result)
			CASPAR_LOG(warning) << print() << " Failed to seek frame.";
		return result;
	}
		
	aligned_buffer get_video_packet()
	{
		return get_packet(video_packet_buffer_);
	}

	aligned_buffer get_audio_packet()
	{
		return get_packet(audio_packet_buffer_);
	}
	
	aligned_buffer get_packet(tbb::concurrent_bounded_queue<std::shared_ptr<aligned_buffer>>& buffer)
	{
		cond_.notify_all();
		std::shared_ptr<aligned_buffer> packet;
		return buffer.try_pop(packet) ? std::move(*packet) : aligned_buffer();
	}

	bool is_eof() const
	{
		return !executor_.is_running() && video_packet_buffer_.empty() && audio_packet_buffer_.empty();
	}
		
	double fps() const
	{
		return static_cast<double>(video_codec_context_->time_base.den) / static_cast<double>(video_codec_context_->time_base.num);
	}

	std::wstring print() const
	{
		return (parent_printer_ ? parent_printer_() + L"/" : L"") + L"input";
	}
};

input::input(const safe_ptr<diagnostics::graph>& graph, const std::wstring& filename, bool loop, const printer& parent_printer) : impl_(new implementation(graph, filename, loop, parent_printer)){}
const std::shared_ptr<AVCodecContext>& input::get_video_codec_context() const{return impl_->video_codec_context_;}
const std::shared_ptr<AVCodecContext>& input::get_audio_codec_context() const{return impl_->audio_codex_context_;}
bool input::is_eof() const{return impl_->is_eof();}
aligned_buffer input::get_video_packet(){return impl_->get_video_packet();}
aligned_buffer input::get_audio_packet(){return impl_->get_audio_packet();}
double input::fps() const { return impl_->fps(); }
}}}