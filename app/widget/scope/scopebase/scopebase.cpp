/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2021 Olive Team

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

***/

#include "scopebase.h"

#include "config/config.h"

namespace olive {

#define super ManagedDisplayWidget

ScopeBase::ScopeBase(QWidget* parent) :
  super(parent),
  buffer_(nullptr)
{
  EnableDefaultContextMenu();
}

void ScopeBase::SetBuffer(Frame *frame)
{
  buffer_ = frame;

  UploadTextureFromBuffer();
}

void ScopeBase::showEvent(QShowEvent* e)
{
  super::showEvent(e);

  UploadTextureFromBuffer();
}

void ScopeBase::DrawScope(TexturePtr managed_tex, QVariant pipeline)
{
  ShaderJob job;

  job.InsertValue(QStringLiteral("ove_maintex"), NodeValue(NodeValue::kTexture, QVariant::fromValue(managed_tex)));

  renderer()->Blit(pipeline, job, VideoParams(width(), height(),
                                              static_cast<VideoParams::Format>(Config::Current()["OfflinePixelFormat"].toInt()),
                                              VideoParams::kInternalChannelCount));
}

void ScopeBase::UploadTextureFromBuffer()
{
  if (!isVisible()) {
    return;
  }

  if (buffer_) {
    makeCurrent();

    if (!texture_
        || texture_->width() != buffer_->width()
        || texture_->height() != buffer_->height()
        || texture_->format() != buffer_->format()) {
      texture_ = nullptr;
      managed_tex_ = nullptr;

      texture_ = renderer()->CreateTexture(buffer_->video_params(),
                                           buffer_->data(), buffer_->linesize_pixels());
      managed_tex_ = renderer()->CreateTexture(buffer_->video_params());
    } else {
      texture_->Upload(buffer_->data(), buffer_->linesize_pixels());
    }

    doneCurrent();
  }

  update();
}

void ScopeBase::OnInit()
{
  super::OnInit();

  pipeline_ = renderer()->CreateNativeShader(GenerateShaderCode());
}

void ScopeBase::OnPaint()
{
  // Clear display surface
  renderer()->ClearDestination();

  if (buffer_) {
    // Convert reference frame to display space
    if (!texture_ || !managed_tex_) {
      UploadTextureFromBuffer();
      makeCurrent(); // UploadTextureFromBuffer calls "doneCurrent", so we re-call "makeCurrent"
    }
    renderer()->BlitColorManaged(color_service(), texture_, true, managed_tex_.get());

    DrawScope(managed_tex_, pipeline_);
  }
}

void ScopeBase::OnDestroy()
{
  super::OnDestroy();

  managed_tex_ = nullptr;
  texture_ = nullptr;
  pipeline_.clear();
}

}
