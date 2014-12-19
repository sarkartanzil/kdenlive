/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2011 by Jean-Baptiste Mardelle (jb@kdenlive.org)        *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA          *
 ***************************************************************************/

#include "meltjob.h"
#include "kdenlivesettings.h"
#include "doc/kdenlivedoc.h"

#include <QDebug>
#include <QUrl>
#include <klocalizedstring.h>

#include <mlt++/Mlt.h>


static void consumer_frame_render(mlt_consumer, MeltJob * self, mlt_frame frame_ptr)
{
    Mlt::Frame frame(frame_ptr);
    self->emitFrameNumber((int) frame.get_position());
}

MeltJob::MeltJob(ClipType cType, const QString &id, const QStringList &parameters,  const QMap <QString, QString>&extraParams)
    : AbstractClipJob(MLTJOB, cType, id, parameters),
    addClipToProject(0),
    m_consumer(NULL),
    m_producer(NULL),
    m_profile(NULL),
    m_filter(NULL),
    m_showFrameEvent(NULL),
    m_length(0),
    m_extra(extraParams)
{
    m_jobStatus = JobWaiting;
    m_params = parameters;
    description = i18n("Process clip");
    QString consum = m_params.at(5);
    if (consum.contains(QLatin1Char(':')))
        m_dest = consum.section(QLatin1Char(':'), 1);
}

void MeltJob::setProducer(Mlt::Producer *producer, const QUrl &url)
{
    Q_UNUSED(producer)

    //FIX stabilize in proxy clips?
    //m_url = QString::fromUtf8(producer->get("resource"));
    //if (m_url == QLatin1String("<playlist>") || m_url == QLatin1String("<tractor>") || m_url == QLatin1String("<producer>"))
        m_url = url.path();
}

void MeltJob::startJob()
{
    if (m_url.isEmpty()) {
        m_errorMessage.append(i18n("No producer for this clip."));
        setStatus(JobCrashed);
        return;
    }
    int in = m_params.takeFirst().toInt();
    if (in > 0 && !m_extra.contains(QLatin1String("offset"))) m_extra.insert(QLatin1String("offset"), QString::number(in));
    int out = m_params.takeFirst().toInt();
    QString producerParams =m_params.takeFirst(); 
    QString filter = m_params.takeFirst();
    QString filterParams = m_params.takeFirst();
    QString consumer = m_params.takeFirst();
    if (consumer.contains(QLatin1Char(':'))) m_dest = consumer.section(QLatin1Char(':'), 1);
    QString consumerParams = m_params.takeFirst();
    
    // optional params
    int startPos = -1;
    if (!m_params.isEmpty()) startPos = m_params.takeFirst().toInt();
    int track = -1;
    if (!m_params.isEmpty()) track = m_params.takeFirst().toInt();
    if (!m_extra.contains(QLatin1String("finalfilter")))
        m_extra.insert(QLatin1String("finalfilter"), filter);

    if (out != -1 && out <= in) {
        m_errorMessage.append(i18n("Clip zone undefined (%1 - %2).", in, out));
        setStatus(JobCrashed);
        return;
    }
    if (m_extra.contains(QLatin1String("producer_profile"))) {
	m_profile = new Mlt::Profile;
	m_profile->set_explicit(false);
    }
    else {
	m_profile = new Mlt::Profile(KdenliveSettings::current_profile().toUtf8().constData());
    }
    if (m_extra.contains(QLatin1String("resize_profile"))) {
    m_profile->set_height(m_extra.value(QLatin1String("resize_profile")).toInt());
	m_profile->set_width(m_profile->height() * m_profile->sar());
    }
    if (out == -1) {
	m_producer = new Mlt::Producer(*m_profile,  m_url.toUtf8().constData());
	if (m_producer) m_length = m_producer->get_length();
    }
    else {
	Mlt::Producer *tmp = new Mlt::Producer(*m_profile,  m_url.toUtf8().constData());
        if (tmp) m_producer = tmp->cut(in, out);
	delete tmp;
	if (m_producer) m_length = m_producer->get_playtime();
    }
    if (!m_producer || !m_producer->is_valid()) {
	// Clip was removed or something went wrong, Notify user?
	//m_errorMessage.append(i18n("Invalid clip"));
        setStatus(JobCrashed);
	return;
    }
    if (m_extra.contains(QLatin1String("producer_profile"))) {
	m_profile->from_producer(*m_producer);
	m_profile->set_explicit(true);
    }
    QStringList list = producerParams.split(QLatin1Char(' '), QString::SkipEmptyParts);
    foreach(const QString &data, list) {
        if (data.contains(QLatin1Char('='))) {
            m_producer->set(data.section(QLatin1Char('='), 0, 0).toUtf8().constData(), data.section(QLatin1Char('='), 1, 1).toUtf8().constData());
        }
    }
    if (consumer.contains(QLatin1String(":"))) {
        m_consumer = new Mlt::Consumer(*m_profile, consumer.section(QLatin1Char(':'), 0, 0).toUtf8().constData(), consumer.section(QLatin1Char(':'), 1).toUtf8().constData());
    }
    else {
        m_consumer = new Mlt::Consumer(*m_profile, consumer.toUtf8().constData());
    }
    if (!m_consumer || !m_consumer->is_valid()) {
        m_errorMessage.append(i18n("Cannot create consumer %1.", consumer));
        setStatus(JobCrashed);
        return;
    }

    //m_consumer->set("terminate_on_pause", 1 );
    //m_consumer->set("eof", "pause" );
    m_consumer->set("real_time", -KdenliveSettings::mltthreads() );


    list = consumerParams.split(QLatin1Char(' '), QString::SkipEmptyParts);
    foreach(const QString &data, list) {
        if (data.contains(QLatin1Char('='))) {
            //qDebug()<<"// filter con: "<<data;
            m_consumer->set(data.section(QLatin1Char('='), 0, 0).toUtf8().constData(), data.section(QLatin1Char('='), 1, 1).toUtf8().constData());
        }
    }
    
    m_filter = new Mlt::Filter(*m_profile, filter.toUtf8().data());
    if (!m_filter || !m_filter->is_valid()) {
	m_errorMessage = i18n("Filter %1 crashed", filter);
        setStatus(JobCrashed);
	return;
    }
    list = filterParams.split(QLatin1Char(' '), QString::SkipEmptyParts);
    foreach(const QString &data, list) {
        if (data.contains(QLatin1Char('='))) {
            //qDebug()<<"// filter p: "<<data;
            m_filter->set(data.section(QLatin1Char('='), 0, 0).toUtf8().constData(), data.section(QLatin1Char('='), 1, 1).toUtf8().constData());
        }
    }
    Mlt::Tractor tractor;
    Mlt::Playlist playlist;
    playlist.append(*m_producer);
    tractor.set_track(playlist, 0);
    m_consumer->connect(tractor);
    m_producer->set_speed(0);
    m_producer->seek(0);
    m_producer->attach(*m_filter);
    m_showFrameEvent = m_consumer->listen("consumer-frame-show", this, (mlt_listener) consumer_frame_render);
    m_producer->set_speed(1);
    m_consumer->run();
    
    QMap <QString, QString> jobResults;
    if (m_jobStatus != JobAborted && m_extra.contains(QLatin1String("key"))) {
    QString result = QString::fromLatin1(m_filter->get(m_extra.value(QLatin1String("key")).toUtf8().constData()));
    jobResults.insert(m_extra.value(QLatin1String("key")), result);
    }
    if (!jobResults.isEmpty() && m_jobStatus != JobAborted) {
	emit gotFilterJobResults(m_clipId, startPos, track, jobResults, m_extra);
    }
    if (m_jobStatus == JobAborted || m_jobStatus == JobWorking) m_jobStatus = JobDone;
}


MeltJob::~MeltJob()
{
    delete m_showFrameEvent;
    delete m_filter;
    delete m_producer;
    delete m_consumer;
    delete m_profile;
}

const QString MeltJob::destination() const
{
    return m_dest;
}

stringMap MeltJob::cancelProperties()
{
    QMap <QString, QString> props;
    return props;
}

const QString MeltJob::statusMessage()
{
    QString statusInfo;
    switch (m_jobStatus) {
        case JobWorking:
            statusInfo = description;
            break;
        case JobWaiting:
            statusInfo = i18n("Waiting to process clip");
            break;
        default:
            break;
    }
    return statusInfo;
}

void MeltJob::emitFrameNumber(int pos)
{
    if (m_length > 0) {
        emit jobProgress(m_clipId, (int) (100 * pos / m_length), jobType);
    }
}

bool MeltJob::isProjectFilter() const
{
    return m_extra.contains(QLatin1String("projecttreefilter"));
}

void MeltJob::setStatus(ClipJobStatus status)
{
    m_jobStatus = status;
    if (status == JobAborted && m_consumer) m_consumer->stop();
}



