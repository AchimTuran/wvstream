/*
* DashStream.cpp
*****************************************************************************
* Copyright(C) 2015, liberty_developer
*
* Email: liberty.developer@xmail.net
*
* This source code and its use and distribution, is subject to the terms
* and conditions of the applicable license agreement.
*****************************************************************************/

#include "DASHStream.h"
#include "../../libcurl/include/curl/curl.h"

#include <iostream>
#include <cstring>
#include "../oscompat.h"

using namespace dash;

DASHStream::DASHStream(DASHTree &tree, DASHTree::StreamType type)
	:tree_(tree)
	, type_(type)
	, observer_(0)
	, current_period_(0)
	, current_adp_(0)
	, current_rep_(0)
	, download_speed_(0)
	, curl_handle_(0)
{
}

/*----------------------------------------------------------------------
|   curl initialization
+---------------------------------------------------------------------*/

static size_t curl_fwrite_init(void *buffer, size_t size, size_t nmemb, void *dest)
{
	std::string *init(reinterpret_cast<std::string *>(dest));
	*init += std::string((const char *)buffer, size*nmemb);
	return size*nmemb;
}

bool DASHStream::download_segment()
{
	segment_buffer_.clear();
	char rangebuf[128];
	sprintf(rangebuf, "/range/%" PRIu64 "-%" PRIu64, current_seg_->range_begin_, current_seg_->range_end_);
	curl_easy_setopt(curl_handle_, CURLOPT_URL, (current_rep_->url_ + rangebuf).c_str());
	/* Define our callback to get called when there's data to be written */
	curl_easy_setopt(curl_handle_, CURLOPT_WRITEFUNCTION, curl_fwrite_init);
	/* Set a pointer to our struct to pass to the callback */
	curl_easy_setopt(curl_handle_, CURLOPT_WRITEDATA, &segment_buffer_);
	segment_read_pos_ = 0;
	absolute_position_ = current_seg_->range_begin_;
	CURLcode ret = curl_easy_perform(curl_handle_);
	if (ret == CURLE_OK)
	{
		curl_easy_getinfo(curl_handle_, CURLINFO_SPEED_DOWNLOAD, &download_speed_);
		return true;
	}
	return false;
}

bool DASHStream::prepare_stream(const uint32_t width, const uint32_t height, const char *lang, uint32_t fixed_bandwidth)
{
	language_ = lang ? lang : "";
	width_ = width;
	height_ = height;
	fixed_bandwidth_ = fixed_bandwidth;

	stopped_ = false;

	current_period_ = tree_.periods_.empty() ? 0 : tree_.periods_[0];
	if (!current_period_)
		return false;

	download_speed_ = tree_.get_download_speed();

	return select_stream();
}

bool DASHStream::start_stream(const uint32_t seg_offset)
{
	segment_buffer_.clear();
	current_seg_ = current_rep_->get_segment(seg_offset);
	if (!current_seg_ || !current_rep_->get_next_segment(current_seg_))
		stopped_ = true;
	absolute_position_ = current_rep_->get_next_segment(current_seg_)->range_begin_;
	return true;
}

uint32_t DASHStream::read(void* buffer, uint32_t  bytesToRead)
{
	if (stopped_)
		return 0;

	if (segment_read_pos_ >= segment_buffer_.size())
	{
		current_seg_ = current_rep_->get_next_segment(current_seg_);
		if (!download_segment() || segment_buffer_.empty())
			return 0;
	}
	uint32_t avail = segment_buffer_.size() - segment_read_pos_;
	if (avail > bytesToRead)
		avail = bytesToRead;
	memcpy(buffer, segment_buffer_.data() + segment_read_pos_, avail);

	segment_read_pos_ += avail;
	absolute_position_ += avail;

	return avail;
}

bool DASHStream::seek(uint64_t const pos)
{
	// we seek only in the current segment
	if (pos >= absolute_position_ - segment_read_pos_)
	{
		segment_read_pos_ = static_cast<uint32_t>(pos - (absolute_position_ - segment_read_pos_));
		if (segment_read_pos_ > segment_buffer_.size())
		{
			segment_read_pos_ = static_cast<uint32_t>(segment_buffer_.size());
			return false;
		}
		absolute_position_ = pos;
		return true;
	}
	return false;
}

bool DASHStream::select_stream(bool force)
{
	const DASHTree::Representation *new_rep(0), *min_rep(0);
	const DASHTree::AdaptationSet *new_adp(0), *min_adp(0);

	if (force && absolute_position_ == 0) //already selected
		return true;

	uint32_t bandwidth = fixed_bandwidth_;
	if (!bandwidth)
		bandwidth = static_cast<uint32_t>(download_speed_*(type_ == DASHTree::VIDEO ? 7.2 : 0.8)); //Bandwith split 90 / 10
	else
		bandwidth = static_cast<uint32_t>(bandwidth *(type_ == DASHTree::VIDEO ? 0.9 : 0.1));
	std::string lang(language_), avail_lang;

NEXTLANG:
	for (std::vector<DASHTree::AdaptationSet*>::const_iterator ba(current_period_->adaptationSets_.begin()), ea(current_period_->adaptationSets_.end()); ba != ea; ++ba)
	{
		if ((*ba)->type_ == type_ && (lang.empty() || stricmp(lang.c_str(), (*ba)->language_.c_str()) == 0))
		{
			for (std::vector<DASHTree::Representation*>::const_iterator br((*ba)->repesentations_.begin()), er((*ba)->repesentations_.end()); br != er; ++br)
			{
				if ((*br)->width_ <= width_ && (*br)->height_ <= height_ && (*br)->bandwidth_ < bandwidth && (*br)->codecs_ != "ec-3"
					&& (!new_rep || ((*br)->bandwidth_ > new_rep->bandwidth_)))
				{
					new_adp = (*ba);
					new_rep = (*br);
				}
				else if (!min_rep || (*br)->bandwidth_ < min_rep->bandwidth_)
				{
					min_adp = (*ba);
					min_rep = (*br);
				}
			}
		}
		else if ((*ba)->type_ == type_)
			avail_lang = (*ba)->language_;
	}
	if (!new_rep && !min_rep && !avail_lang.empty())
	{
		lang = avail_lang;
		goto NEXTLANG;
	}

	if (!new_rep)
	{
		new_adp = min_adp;
		new_rep = min_rep;
	}

	if (!force && new_rep == current_rep_)
		return false;

	uint32_t segid(current_rep_ ? current_rep_->get_segment_pos(current_seg_) : 0);

	if (curl_handle_)
		clear();

	current_adp_ = new_adp;
	current_rep_ = new_rep;

	if (observer_)
		observer_->OnStreamChange(this, segid);

	curl_handle_ = curl_easy_init();
	/* enable TCP keep - alive for this transfer */
	curl_easy_setopt(curl_handle_, CURLOPT_TCP_KEEPALIVE, 1L);
	/* keep-alive idle time to 120 seconds */
	curl_easy_setopt(curl_handle_, CURLOPT_TCP_KEEPIDLE, 120L);
	/* interval time between keep-alive probes: 60 seconds */
	curl_easy_setopt(curl_handle_, CURLOPT_TCP_KEEPINTVL, 60L);

	/* lets download the initialization */
	if (current_seg_ = current_rep_->get_initialization())
		return download_segment();

	stopped_ = true;
	return false;
}

void DASHStream::info(std::ostream &s)
{
	static const char* ts[4] = { "NoType", "Video", "Audio", "Text" };
	s << ts[type_] << " representation: " << current_rep_->url_.substr(current_rep_->url_.find_last_of('/') + 1) << " bandwidth: " << current_rep_->bandwidth_ << std::endl;
}

void DASHStream::clear()
{
	curl_easy_cleanup(curl_handle_);
	curl_handle_ = 0;
	current_adp_ = 0;
	current_rep_ = 0;
}

DASHStream::~DASHStream()
{
	clear();
}
