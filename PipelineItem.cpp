/*
 * App To Record systems using xdg-desktop-portal
 * Copyright 2020 Aleix Pol Gonzalez <aleixpol@kde.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License or (at your option) version 3 or any later version
 * accepted by the membership of KDE e.V. (or its successor approved
 * by the membership of KDE e.V.), which shall act as a proxy
 * defined in Section 14 of version 3 of the license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PipelineItem.h"
#include <QDebug>

static gboolean pipelineWatch(GstBus     */*bus*/, GstMessage *message, gpointer user_data)
{
    PipelineItem* wc = static_cast<PipelineItem*>(user_data);
    wc->onBusMessage(message);
    return G_SOURCE_CONTINUE;
}

template <typename T>
using GstPointer = QScopedPointer<T, GstPointerCleanup<T>>;

static QString debugMessage(GstMessage* msg)
{
    gchar *debug = nullptr;
    GError *e = nullptr;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING) {
        gst_message_parse_warning(msg, &e, &debug);
    } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        gst_message_parse_error(msg, &e, &debug);
    }
    if (!debug)
        return {};

    if (e) {
        qWarning() << "error debugMessage:" << e->message;
        g_error_free (e);
    }
    const auto ret = QString::fromUtf8(debug);
    g_free(debug);
    return ret;
}

template <class T>
GstState pipelineCurrentState(const T &pipe)
{
    GstState currentState, pendingState;
    GstStateChangeReturn result = gst_element_get_state(GST_ELEMENT(pipe.data()), &currentState, &pendingState, GST_CLOCK_TIME_NONE );
    if(result == GST_STATE_CHANGE_FAILURE)
        qDebug() << "broken state";
    return currentState;
}

PipelineItem::PipelineItem(QObject* parent)
    : QObject(parent)
    , m_pipeline(gst_pipeline_new("recordme"))
{
    gst_bus_add_watch (gst_pipeline_get_bus(GST_PIPELINE(m_pipeline.data())), &pipelineWatch, this);
}

void PipelineItem::componentComplete()
{
    GstElement* source, *sink;
    GstElement* elements[] = {
        source = gst_element_factory_make("pipewiresrc", "source"),
//         source = gst_element_factory_make("videotestsrc", "source"),
        gst_element_factory_make("glupload", "convert"),
        gst_element_factory_make("glcolorconvert", "convert2"),
        sink = gst_element_factory_make("qmlglsink", "sink"),
    };

    gst_bin_add_many (GST_BIN (m_pipeline.data()), elements[0], elements[1], elements[2], elements[3], nullptr);
    if (!gst_element_link(elements[0], elements[1]) || !gst_element_link(elements[1], elements[2]) || !gst_element_link(elements[2], elements[3])) {
        qCritical() << "Elements could not be linked.";
        return;
    }

//     Q_ASSERT(fd);
//     qDebug() << "playing..." << m_nodeid;
    const auto str = QByteArray::number(m_nodeid);
    g_object_set(source, "path", str.constData(), nullptr);
//     g_object_set(source, "fd", m_fd, nullptr);

    g_object_set(sink, "qos", 0, "sync", 0,
                       "widget", m_widget,
                       nullptr);
}

PipelineItem::~PipelineItem()
{
    if (m_pipeline)
        gst_element_set_state(m_pipeline.data(), GST_STATE_NULL);
}

void PipelineItem::onBusMessage(GstMessage* message)
{
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS: //End of stream. We reached the end of the file.
        setPlaying(false);
        break;
    case GST_MESSAGE_ERROR:  {//Some error occurred.
        qCritical() << "error on:" << m_pipeline << debugMessage(message);
        gst_element_set_state(GST_ELEMENT(m_pipeline.data()), GST_STATE_NULL);
        m_pipeline.reset(nullptr);
        Q_EMIT failed();
    }   break;
    default:
        break;
    }
}

void PipelineItem::setPlaying(bool playing)
{
    qDebug() << "playing..." << m_playing << "to" << playing << m_pipeline;
    if (playing != m_playing) {
        m_playing = playing;
        Q_EMIT playingChanged(playing);
    }

    if (m_pipeline) {
        if (playing) {
            auto ret = gst_element_set_state(GST_ELEMENT(m_pipeline.data()), GST_STATE_READY);
            Q_ASSERT(ret == GST_STATE_CHANGE_SUCCESS);
        }

        qDebug() << "playing" << playing <<
        gst_element_set_state(GST_ELEMENT(m_pipeline.data()), playing ? GST_STATE_PLAYING : GST_STATE_NULL);
    }
}

bool PipelineItem::playing() const
{
    return m_pipeline && pipelineCurrentState(m_pipeline) == GST_STATE_PLAYING;
}
