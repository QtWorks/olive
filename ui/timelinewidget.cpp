#include "timelinewidget.h"

#include "panels/panels.h"
#include "io/config.h"
#include "project/sequence.h"
#include "project/clip.h"
#include "panels/project.h"
#include "panels/timeline.h"
#include "io/media.h"
#include "ui/sourcetable.h"
#include "panels/effectcontrols.h"

#include "effects/effects.h"

#include <QPainter>
#include <QColor>
#include <QDebug>
#include <QMouseEvent>
#include <QObject>
#include <QVariant>
#include <QPointF>

TimelineWidget::TimelineWidget(QWidget *parent) : QWidget(parent)
{
	bottom_align = false;
	setMouseTracking(true);
    track_height = 40;

	setAcceptDrops(true);
}

bool same_sign(int a, int b) {
	return (a < 0) == (b < 0);
}

void TimelineWidget::resizeEvent(QResizeEvent*) {
    if (panel_timeline->sequence != NULL) redraw_clips();
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent *event) {
	if (static_cast<SourceTable*>(event->source()) == panel_project->source_table) {
		QPoint pos = event->pos();
		event->accept();
		QList<QTreeWidgetItem*> items = panel_project->source_table->selectedItems();
		long entry_point = getFrameFromScreenPoint(pos.x(), false);
		panel_timeline->drag_frame_start = entry_point + getFrameFromScreenPoint(50, false);
		panel_timeline->drag_track_start = (bottom_align) ? -1 : 0;
		for (int i=0;i<items.size();i++) {
			bool ignore_infinite_length = false;
			Media* m = reinterpret_cast<Media*>(items.at(i)->data(0, Qt::UserRole + 1).value<quintptr>());
			long duration = m->get_length_in_frames(panel_timeline->sequence->frame_rate);
			Ghost g = {NULL, entry_point, entry_point + duration};
			g.media = m;
			g.clip_in = 0;
			for (int j=0;j<m->audio_tracks.size();j++) {
				g.track = j;
				g.media_stream = &m->audio_tracks[j];
				ignore_infinite_length = true;
				panel_timeline->ghosts.append(g);
			}
			for (int j=0;j<m->video_tracks.size();j++) {
				g.track = -1-j;
				g.media_stream = &m->video_tracks[j];
				if (m->video_tracks[j].infinite_length && !ignore_infinite_length) g.out = g.in + 100;
				panel_timeline->ghosts.append(g);
			}
			entry_point += duration;
		}
		init_ghosts();
		panel_timeline->importing = true;
	}
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent *event) {
	if (panel_timeline->importing) {
		QPoint pos = event->pos();
		update_ghosts(pos);
		panel_timeline->repaint_timeline();
	}
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent*) {
	if (panel_timeline->importing) {
		panel_timeline->ghosts.clear();
		panel_timeline->importing = false;
		panel_timeline->repaint_timeline();
	}
}

void TimelineWidget::dropEvent(QDropEvent* event) {
	if (panel_timeline->importing) {
		event->accept();

		for (int i=0;i<panel_timeline->ghosts.size();i++) {
			const Ghost& g = panel_timeline->ghosts.at(i);

			panel_timeline->sequence->delete_area(g.in, g.out, g.track);

			Clip& c = panel_timeline->sequence->new_clip();
			c.media = g.media;
			c.media_stream = g.media_stream;
			c.timeline_in = g.in;
			c.timeline_out = g.out;
			c.clip_in = g.clip_in;
			c.color_r = 128;
			c.color_g = 128;
			c.color_b = 192;
			c.sequence = panel_timeline->sequence;
			c.track = g.track;
			c.name = c.media->name;

			if (c.track < 0) {
				// add default video effects
                c.effects.append(create_effect(VIDEO_TRANSFORM_EFFECT, &c));
			} else {
				// add default audio effects
                c.effects.append(create_effect(AUDIO_VOLUME_EFFECT, &c));
                c.effects.append(create_effect(AUDIO_PAN_EFFECT, &c));
			}
		}

		panel_timeline->ghosts.clear();
		panel_timeline->importing = false;
        panel_timeline->snapped = false;

		panel_timeline->redraw_all_clips();
	}
}

void TimelineWidget::mousePressEvent(QMouseEvent *event) {
	QPoint pos = event->pos();

	panel_timeline->drag_frame_start = getFrameFromScreenPoint(pos.x(), false);
	panel_timeline->drag_track_start = getTrackFromScreenPoint(pos.y());
	int clip_index = panel_timeline->trim_target;
	if (clip_index == -1) clip_index = getClipIndexFromCoords(panel_timeline->drag_frame_start, panel_timeline->drag_track_start);

	switch (panel_timeline->tool) {
	case TIMELINE_TOOL_POINTER:
	case TIMELINE_TOOL_RIPPLE:
	{
		if (clip_index >= 0) {
			if (panel_timeline->is_clip_selected(clip_index)) {
				// TODO if shift is down, deselect it
			} else {
				// if "shift" is not down
				if (!(event->modifiers() & Qt::ShiftModifier)) {
					panel_timeline->selections.clear();
				}

				Clip& clip = panel_timeline->sequence->get_clip(clip_index);
				panel_timeline->selections.append({clip.timeline_in, clip.timeline_out, clip.track});
			}
			panel_timeline->moving_init = true;
		} else {
			panel_timeline->selections.clear();
		}
		panel_timeline->repaint_timeline();
	}
		break;
	case TIMELINE_TOOL_EDIT:
		panel_timeline->seek(panel_timeline->drag_frame_start);
		panel_timeline->selecting = true;
		panel_timeline->repaint_timeline();
		break;
	case TIMELINE_TOOL_RAZOR:
	{
		if (clip_index >= 0) {
			panel_timeline->sequence->split_clip(clip_index, panel_timeline->drag_frame_start);
		}
		panel_timeline->splitting = true;
		panel_timeline->redraw_all_clips();
	}
		break;
	}
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent *event) {
	if (panel_timeline->moving_proc) {
		if (event->modifiers() & Qt::AltModifier) { // if holding alt, duplicate rather than move
			// duplicate clips
			for (int i=0;i<panel_timeline->ghosts.size();i++) {
				Ghost& g = panel_timeline->ghosts[i];
				Clip& c = panel_timeline->sequence->insert_clip(*g.clip);

				c.timeline_in = g.in;
				c.timeline_out = g.out;
				c.track = g.track;
				c.undeletable = true;

				// step 2 - delete anything that exists in area that clip is moving to
				panel_timeline->sequence->delete_area(g.in, g.out, g.track);

				c.undeletable = false;
			}
		} else {
			// move clips
			// TODO can we do this better than 3 consecutive for loops?
			for (int i=0;i<panel_timeline->ghosts.size();i++) {
				// step 1 - set clips that are moving to "undeletable" (to avoid step 2 deleting any part of them)
				panel_timeline->ghosts[i].clip->undeletable = true;
			}
			for (int i=0;i<panel_timeline->ghosts.size();i++) {
				Ghost& g = panel_timeline->ghosts[i];

				if (panel_timeline->tool == TIMELINE_TOOL_POINTER) {
					// step 2 - delete anything that exists in area that clip is moving to
					// note: ripples are non-destructive so this is pointer-tool exclusive
					panel_timeline->sequence->delete_area(g.in, g.out, g.track);
				}

				// step 3 - move clips
				g.clip->timeline_in = g.in;
				g.clip->timeline_out = g.out;
				g.clip->track = g.track;
				g.clip->clip_in = g.clip_in;
			}
			for (int i=0;i<panel_timeline->ghosts.size();i++) {
				// step 4 - set clips back to deletable
				panel_timeline->ghosts[i].clip->undeletable = false;
			}
		}

		// ripple ops
		if (panel_timeline->tool == TIMELINE_TOOL_RIPPLE) {
			long ripple_length, ripple_point;
			if (panel_timeline->trim_in) {
				ripple_length = panel_timeline->ghosts.at(0).old_in - panel_timeline->ghosts.at(0).in;
				ripple_point = (ripple_length < 0) ? panel_timeline->ghosts.at(0).old_in : panel_timeline->ghosts.at(0).in;
			} else {
				ripple_length = panel_timeline->ghosts.at(0).old_out - panel_timeline->ghosts.at(0).out;
				ripple_point = (ripple_length < 0) ? panel_timeline->ghosts.at(0).old_out : panel_timeline->ghosts.at(0).out;
			}
			for (int i=0;i<panel_timeline->ghosts.size();i++) {
				long comp_point;
				if (panel_timeline->trim_in) {
					comp_point = (ripple_length < 0) ? panel_timeline->ghosts.at(i).old_in : panel_timeline->ghosts.at(i).in;
				} else {
					comp_point = (ripple_length < 0) ? panel_timeline->ghosts.at(i).old_out : panel_timeline->ghosts.at(i).out;
				}
				if (ripple_point > comp_point) ripple_point = comp_point;
			}
			if (panel_timeline->trim_in) {
				panel_timeline->ripple(ripple_point, ripple_length);
			} else {
				panel_timeline->ripple(ripple_point, -ripple_length);
			}
		}

		panel_timeline->redraw_all_clips();
	}

	// destroy all ghosts
	panel_timeline->ghosts.clear();

	panel_timeline->selecting = false;
	panel_timeline->moving_proc = false;
	panel_timeline->moving_init = false;
	panel_timeline->splitting = false;
    panel_timeline->snapped = false;
	pre_clips.clear();
	post_clips.clear();

	// find out how many clips are selected
	bool single_select = false;
	int selected_clip = 0;
	for (int i=0;i<panel_timeline->sequence->clip_count();i++) {
		if (panel_timeline->is_clip_selected(i)) {
			if (!single_select) {
				// found ONE selected clip
				selected_clip = i;
				single_select = true;
			} else {
				// more than one clip is selected
				single_select = false;
				break;
			}
		}
	}
	if (single_select) {
		panel_effect_controls->set_clip(&panel_timeline->sequence->get_clip(selected_clip));
	} else {
		panel_effect_controls->set_clip(NULL);
	}
}

void TimelineWidget::init_ghosts() {
	for (int i=0;i<panel_timeline->ghosts.size();i++) {
		Ghost& g = panel_timeline->ghosts[i];
		g.old_in = g.in;
		g.old_out = g.out;
		g.old_track = g.track;
		g.old_clip_in = g.clip_in;

		if (panel_timeline->trim_target > -1) {
			// used for trim ops
			g.ghost_length = g.old_out - g.old_in;
			g.media_length = g.clip->media->get_length_in_frames(panel_timeline->sequence->frame_rate);
		}
	}
	for (int i=0;i<panel_timeline->selections.size();i++) {
		Selection& s = panel_timeline->selections[i];
		s.old_in = s.in;
		s.old_out = s.out;
		s.old_track = s.track;
	}
}

bool subvalidate_snapping(Ghost& g, long* frame_diff, long snap_point) {
    int snap_range = 15;
    long in_validator = g.old_in + *frame_diff - snap_point;
    long out_validator = g.old_out + *frame_diff - snap_point;

    if (in_validator > -snap_range && in_validator < snap_range) {
        *frame_diff -= in_validator;
        panel_timeline->snap_point = snap_point;
        panel_timeline->snapped = true;
        return true;
    } else if (out_validator > -snap_range && out_validator < snap_range) {
        *frame_diff -= out_validator;
        panel_timeline->snap_point = snap_point;
        panel_timeline->snapped = true;
        return true;
    }
    return false;
}

void validate_snapping(Ghost& g, long* frame_diff) {
    panel_timeline->snapped = false;
    if (panel_timeline->snapping) {
        if (!subvalidate_snapping(g, frame_diff, panel_timeline->playhead)) {
            for (int j=0;j<panel_timeline->sequence->clip_count();j++) {
                Clip& c = panel_timeline->sequence->get_clip(j);
                if (!subvalidate_snapping(g, frame_diff, c.timeline_in)) {
                    subvalidate_snapping(g, frame_diff, c.timeline_out);
                }
                if (panel_timeline->snapped) break;
            }
        }
    }
}

void TimelineWidget::update_ghosts(QPoint& mouse_pos) {
	int mouse_track = getTrackFromScreenPoint(mouse_pos.y());
	long frame_diff = getFrameFromScreenPoint(mouse_pos.x(), false) - panel_timeline->drag_frame_start;
	int track_diff = mouse_track - panel_timeline->drag_track_start;

	long validator;
	if (panel_timeline->trim_target > -1) {
		// trim ops

		// validate ghosts
		for (int i=0;i<panel_timeline->ghosts.size();i++) {
			Ghost& g = panel_timeline->ghosts[i];

			if (panel_timeline->trim_in) {
				// prevent clip length from being less than 1 frame long
				validator = g.ghost_length - frame_diff;
				if (validator < 1) frame_diff -= (1 - validator);

				// prevent timeline in from going below 0
				validator = g.old_in + frame_diff;
				if (validator < 0) frame_diff -= validator;

				if (!g.clip->media_stream->infinite_length) {
					// prevent clip_in from going below 0
					validator = g.old_clip_in + frame_diff;
					if (validator < 0) frame_diff -= validator;
				}

				// ripple ops
				if (panel_timeline->tool == TIMELINE_TOOL_RIPPLE) {
					for (int j=0;j<post_clips.size();j++) {
						// prevent any rippled clip from going below 0
						Clip* post = post_clips.at(j);
						validator = post->timeline_in - frame_diff;
						if (validator < 0) frame_diff += validator;

						// prevent any post-clips colliding with pre-clips
						for (int k=0;k<pre_clips.size();k++) {
							Clip* pre = pre_clips.at(k);
							if (pre->track == post->track) {
								validator = post->timeline_in - frame_diff - pre->timeline_out;
								if (validator < 0) frame_diff += validator;
							}
						}
					}
                }
			} else {
				// prevent clip length from being less than 1 frame long
				validator = g.ghost_length + frame_diff;
				if (validator < 1) frame_diff += (1 - validator);

				if (!g.clip->media_stream->infinite_length) {
					// prevent clip length exceeding media length
					validator = g.ghost_length + frame_diff;
					if (validator > g.media_length) frame_diff -= validator - g.media_length;
				}

				// ripple ops
				if (panel_timeline->tool == TIMELINE_TOOL_RIPPLE) {
					for (int j=0;j<post_clips.size();j++) {
						Clip* post = post_clips.at(j);

						// prevent any post-clips colliding with pre-clips
						for (int k=0;k<pre_clips.size();k++) {
							Clip* pre = pre_clips.at(k);
							if (pre->track == post->track) {
								validator = post->timeline_in + frame_diff - pre->timeline_out;
								if (validator < 0) frame_diff -= validator;
							}
						}
					}
				}
			}

            validate_snapping(g, &frame_diff);
		}

		// resize ghosts
		for (int i=0;i<panel_timeline->ghosts.size();i++) {
			Ghost& g = panel_timeline->ghosts[i];

			if (panel_timeline->trim_in) {
				g.in = g.old_in + frame_diff;
				g.clip_in = g.old_clip_in + frame_diff;
			} else {
				g.out = g.old_out + frame_diff;
			}
		}

		// resize selections
		for (int i=0;i<panel_timeline->selections.size();i++) {
			Selection& s = panel_timeline->selections[i];

			if (panel_timeline->trim_in) {
				s.in = s.old_in + frame_diff;
			} else {
				s.out = s.old_out + frame_diff;
			}
		}
	} else if (panel_timeline->tool == TIMELINE_TOOL_POINTER || panel_timeline->importing) { // only move clips on pointer (not ripple or rolling)
		// validate ghosts
		for (int i=0;i<panel_timeline->ghosts.size();i++) {
			Ghost& g = panel_timeline->ghosts[i];

			// prevent clips from moving below 0 on the timeline
			validator = g.old_in + frame_diff;
			if (validator < 0) frame_diff -= validator;

			// prevent clips from crossing tracks
			if (same_sign(g.old_track, panel_timeline->drag_track_start)) {
				while (!same_sign(g.old_track, g.old_track + track_diff)) {
					if (g.old_track < 0) {
						track_diff--;
					} else {
						track_diff++;
					}
				}
			}

            validate_snapping(g, &frame_diff);
		}

		// move ghosts
		for (int i=0;i<panel_timeline->ghosts.size();i++) {
			Ghost& g = panel_timeline->ghosts[i];
			g.in = g.old_in + frame_diff;
			g.out = g.old_out + frame_diff;

			g.track = g.old_track;

			if (panel_timeline->importing) {
				int abs_track_diff = abs(track_diff);
				if (g.old_track < 0) { // clip is video
					g.track -= abs_track_diff;
				} else { // clip is audio
					g.track += abs_track_diff;
				}
			} else {
				if (same_sign(g.old_track, panel_timeline->drag_track_start)) g.track += track_diff;
			}
		}

		// move selections
		if (!panel_timeline->importing) {
			for (int i=0;i<panel_timeline->selections.size();i++) {
				Selection& s = panel_timeline->selections[i];
				s.in = s.old_in + frame_diff;
				s.out = s.old_out + frame_diff;
				s.track = s.old_track;
				if (panel_timeline->importing) {
					int abs_track_diff = abs(track_diff);
					if (s.old_track < 0) {
						s.track -= abs_track_diff;
					} else {
						s.track += abs_track_diff;
					}
				} else {
					if (same_sign(s.track, panel_timeline->drag_track_start)) s.track += track_diff;
				}
			}
		}
	}
}

void TimelineWidget::mouseMoveEvent(QMouseEvent *event) {
	if (panel_timeline->selecting) {
		QPoint pos = event->pos();

		int current_selection_track = getTrackFromScreenPoint(pos.y());
		long current_selection_frame = getFrameFromScreenPoint(pos.x(), false);;
		int selection_count = 1 + qMax(current_selection_track, panel_timeline->drag_track_start) - qMin(current_selection_track, panel_timeline->drag_track_start);
		if (panel_timeline->selections.size() != selection_count) {
			panel_timeline->selections.resize(selection_count);
		}
		int minimum_selection_track = qMin(current_selection_track, panel_timeline->drag_track_start);
		for (int i=0;i<selection_count;i++) {
			Selection* s = &panel_timeline->selections[i];
			s->track = minimum_selection_track + i;
			long in = panel_timeline->drag_frame_start;
			long out = current_selection_frame;
			s->in = qMin(in, out);
			s->out = qMax(in, out);
		}
		panel_timeline->playhead = qMin(panel_timeline->drag_frame_start, current_selection_frame);
		panel_timeline->repaint_timeline();
	} else if (panel_timeline->moving_init) {
		QPoint pos = event->pos();

		if (panel_timeline->moving_proc) {
			update_ghosts(pos);
		} else {
			// set up movement
			// create ghosts
			for (int i=0;i<panel_timeline->sequence->clip_count();i++) {
				if (panel_timeline->is_clip_selected(i)) {
					Clip& c = panel_timeline->sequence->get_clip(i);
					panel_timeline->ghosts.append({&c, c.timeline_in, c.timeline_out, c.track, c.clip_in});
				}
			}

			// ripple edit prep
			if (panel_timeline->tool == TIMELINE_TOOL_RIPPLE) {
				for (int i=0;i<panel_timeline->ghosts.size();i++) {
					// get clips before and after ripple point
					for (int j=0;j<panel_timeline->sequence->clip_count();j++) {
						// don't cache any currently selected clips
						Clip* c = panel_timeline->ghosts.at(i).clip;
						Clip& cc = panel_timeline->sequence->get_clip(j);
						bool is_selected = false;
						for (int k=0;k<panel_timeline->ghosts.size();k++) {
							if (panel_timeline->ghosts.at(k).clip == &cc) {
								is_selected = true;
								break;
							}
						}

						if (!is_selected) {
							if (cc.timeline_in < c->timeline_in) {
								// add clip to pre-cache UNLESS there is already a clip on that track closer to the ripple point
								bool found = false;
								for (int k=0;k<pre_clips.size();k++) {
									Clip* ccc = pre_clips.at(k);
									if (ccc->track == cc.track) {
										if (ccc->timeline_in < cc.timeline_in) {
											// clip is closer to ripple point than the one in cache, replace it
											ccc = &cc;
										}
										found = true;
									}
								}
								if (!found) {
									// no clip from that track in the cache, add it
									pre_clips.append(&cc);
								}
							} else {
								// add clip to post-cache UNLESS there is already a clip on that track closer to the ripple point
								bool found = false;
								for (int k=0;k<post_clips.size();k++) {
									Clip* ccc = post_clips.at(k);
									if (ccc->track == cc.track) {
										if (ccc->timeline_in > cc.timeline_in) {
											// clip is closer to ripple point than the one in cache, replace it
											ccc = &cc;
										}
										found = true;
									}
								}
								if (!found) {
									// no clip from that track in the cache, add it
									post_clips.append(&cc);
								}
							}
						}
					}
				}

				// debug code - print the information we got
//				qDebug() << "found" << pre_clips.size() << "preceding clips and" << post_clips.size() << "following";
			}

			init_ghosts();

			panel_timeline->moving_proc = true;
		}
		panel_timeline->repaint_timeline();
	} else if (panel_timeline->splitting) {
		QPoint pos = event->pos();

		int track = getTrackFromScreenPoint(pos.y());
		bool repaint = false;
		for (int i=0;i<panel_timeline->sequence->clip_count();i++) {
			if (panel_timeline->sequence->get_clip(i).track == track) {
				panel_timeline->sequence->split_clip(i, panel_timeline->drag_frame_start);
				repaint = true;
			}
		}

		// redraw clips since we changed them
		if (repaint) panel_timeline->redraw_all_clips();
	} else if (panel_timeline->tool == TIMELINE_TOOL_POINTER || panel_timeline->tool == TIMELINE_TOOL_RIPPLE) {
		QPoint pos = event->pos();

		int lim = 5;
		int mouse_track = getTrackFromScreenPoint(pos.y());
		long mouse_frame_lower = getFrameFromScreenPoint(pos.x()-lim, false)-1;
		long mouse_frame_upper = getFrameFromScreenPoint(pos.x()+lim, false)+1;
		bool found = false;
		for (int i=0;i<panel_timeline->sequence->clip_count();i++) {
			Clip& c = panel_timeline->sequence->get_clip(i);
			if (c.track == mouse_track) {
				if (c.timeline_in > mouse_frame_lower && c.timeline_in < mouse_frame_upper) {
					panel_timeline->trim_target = i;
					panel_timeline->trim_in = true;
					found = true;
					break;
				} else if (c.timeline_out > mouse_frame_lower && c.timeline_out < mouse_frame_upper) {
					panel_timeline->trim_target = i;
					panel_timeline->trim_in = false;
					found = true;
					break;
				}
			}
		}
		if (found) {
			setCursor(Qt::SizeHorCursor);
		} else {
			unsetCursor();
			panel_timeline->trim_target = -1;
		}
	} else if (panel_timeline->tool == TIMELINE_TOOL_EDIT || panel_timeline->tool == TIMELINE_TOOL_RAZOR) {
		// redraw because we have a cursor
		panel_timeline->repaint_timeline();
	}
}

int color_brightness(int r, int g, int b) {
	return (0.2126*r + 0.7152*g + 0.0722*b);
}

void TimelineWidget::redraw_clips() {
    // Draw clips
	int panel_width = getScreenPointFromFrame(panel_timeline->sequence->getEndFrame()) + 100;
	setMinimumWidth(panel_width);

    clip_pixmap = QPixmap(panel_width, height());
    clip_pixmap.fill(Qt::transparent);
    QPainter clip_painter(&clip_pixmap);
	int video_track_limit = 0;
	int audio_track_limit = 0;
	for (int i=0;i<panel_timeline->sequence->clip_count();i++) {
		Clip& clip = panel_timeline->sequence->get_clip(i);
		if (is_track_visible(clip.track)) {
			if (clip.track < 0 && clip.track < video_track_limit) { // video clip
				video_track_limit = clip.track;
			} else if (clip.track > audio_track_limit) {
				audio_track_limit = clip.track;
			}

			QRect clip_rect(getScreenPointFromFrame(clip.timeline_in), getScreenPointFromTrack(clip.track), clip.getLength() * panel_timeline->zoom, track_height);
			clip_painter.fillRect(clip_rect, QColor(clip.color_r, clip.color_g, clip.color_b));
			clip_painter.setPen(Qt::white);
			clip_painter.drawLine(clip_rect.bottomLeft(), clip_rect.topLeft());
			clip_painter.drawLine(clip_rect.topLeft(), clip_rect.topRight());
			clip_painter.setPen(QColor(0, 0, 0, 128));
			clip_painter.drawLine(clip_rect.bottomLeft(), clip_rect.bottomRight());
			clip_painter.drawLine(clip_rect.bottomRight(), clip_rect.topRight());

			if (color_brightness(clip.color_r, clip.color_g, clip.color_b) > 160) {
				clip_painter.setPen(Qt::black);
			} else {
				clip_painter.setPen(Qt::white);
			}
			QRect text_rect(clip_rect.left() + CLIP_TEXT_PADDING, clip_rect.top() + CLIP_TEXT_PADDING, clip_rect.width() - CLIP_TEXT_PADDING - CLIP_TEXT_PADDING, clip_rect.height() - CLIP_TEXT_PADDING - CLIP_TEXT_PADDING);
			clip_painter.drawText(text_rect, 0, clip.name, &text_rect);
		}
	}

	// Draw track lines
	if (show_track_lines) {
		clip_painter.setPen(QColor(0, 0, 0, 96));
		audio_track_limit++;
		if (video_track_limit == 0) video_track_limit--;

		if (bottom_align) {
			// only draw lines for video tracks
			for (int i=video_track_limit;i<0;i++) {
				int line_y = getScreenPointFromTrack(i) - 1;
				clip_painter.drawLine(0, line_y, rect().width(), line_y);
			}
		} else {
			// only draw lines for audio tracks
			for (int i=0;i<audio_track_limit;i++) {
				int line_y = getScreenPointFromTrack(i) + track_height;
				clip_painter.drawLine(0, line_y, rect().width(), line_y);
			}
		}
	}

	update();
}

void TimelineWidget::paintEvent(QPaintEvent*) {
	if (panel_timeline->sequence != NULL) {
		QPainter p(this);

        p.drawPixmap(0, 0, minimumWidth(), height(), clip_pixmap);

		// Draw selections
		for (int i=0;i<panel_timeline->selections.size();i++) {
			const Selection& s = panel_timeline->selections.at(i);
			if (is_track_visible(s.track)) {
				int selection_y = getScreenPointFromTrack(s.track);
				int selection_x = getScreenPointFromFrame(s.in);
				p.fillRect(selection_x, selection_y, getScreenPointFromFrame(s.out) - selection_x, track_height, QColor(0, 0, 0, 64));
			}
		}

		// Draw ghosts
		for (int i=0;i<panel_timeline->ghosts.size();i++) {
			const Ghost& g = panel_timeline->ghosts.at(i);
			if (is_track_visible(g.track)) {
				int ghost_x = getScreenPointFromFrame(g.in);
				int ghost_y = getScreenPointFromTrack(g.track);
				int ghost_width = getScreenPointFromFrame(g.out - g.in) - 1;
				int ghost_height = track_height - 1;
				p.setPen(QColor(255, 255, 0));
				for (int j=0;j<GHOST_THICKNESS;j++) {
					p.drawRect(ghost_x+j, ghost_y+j, ghost_width-j-j, ghost_height-j-j);
				}
			}
		}

        // Draw playhead
        p.setPen(Qt::red);
        int playhead_x = getScreenPointFromFrame(panel_timeline->playhead);
        p.drawLine(playhead_x, rect().top(), playhead_x, rect().bottom());

        p.setPen(QColor(0, 0, 0, 64));
        int edge_y = (bottom_align) ? rect().height()-1 : 0;
        p.drawLine(0, edge_y, rect().width(), edge_y);

        // draw snap point
        if (panel_timeline->snapping && panel_timeline->snapped) {
            p.setPen(Qt::white);
            int snap_x = getScreenPointFromFrame(panel_timeline->snap_point);
            p.drawLine(snap_x, 0, snap_x, height());
        }

		// Draw edit cursor
		if (panel_timeline->tool == TIMELINE_TOOL_EDIT || panel_timeline->tool == TIMELINE_TOOL_RAZOR) {
			QPoint mouse_pos = mapFromGlobal(QCursor::pos());
			int track = getTrackFromScreenPoint(mouse_pos.y());
			if (is_track_visible(track)) {
				int cursor_x = getScreenPointFromFrame(getFrameFromScreenPoint(mouse_pos.x(), false));
				int cursor_y = getScreenPointFromTrack(track);

				p.setPen(Qt::gray);
				p.drawLine(cursor_x, cursor_y, cursor_x, cursor_y + track_height);
			}
        }
	}
}

bool TimelineWidget::is_track_visible(int track) {
	return ((bottom_align && track < 0) || (!bottom_align && track >= 0));
}

// **************************************
// screen point <-> frame/track functions
// **************************************

long TimelineWidget::getFrameFromScreenPoint(int x, bool f) {
	float div = (float) x / panel_timeline->zoom;
	if (div < 0) {
		return 0;
	}
	if (f) {
		return floor(div);
	} else {
		return round(div);
	}
}

int TimelineWidget::getTrackFromScreenPoint(int y) {	
	if (bottom_align) {
		y -= rect().bottom();
		if (show_track_lines) y -= 1;
	} else {
		y -= 1;
	}
	int temp_track_height = track_height;
	if (show_track_lines) temp_track_height--;
	return (int)floor((float) (y)/ (float) temp_track_height);
}

int TimelineWidget::getScreenPointFromFrame(long frame) {
	return (int) round(frame*panel_timeline->zoom);
}

int TimelineWidget::getScreenPointFromTrack(int track) {
	int temp_track_height = track_height;
	if (show_track_lines) temp_track_height++;
	int y = track * temp_track_height;

	if (bottom_align) { // video track
		y += rect().bottom();
		if (show_track_lines) y += 1;
	} else { // audio track
		y += 1;
	}
	return y;
}

int TimelineWidget::getClipIndexFromCoords(long frame, int track) {
	for (int i=0;i<panel_timeline->sequence->clip_count();i++) {
		Clip& c = panel_timeline->sequence->get_clip(i);
		if (c.track == track) {
			if (frame >= c.timeline_in && frame < c.timeline_out) {
				return i;
			}
		}
	}
	return -1;
}
