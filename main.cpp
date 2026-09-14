// zer0-kristal — lecteur audio façon Winamp, en C++/Qt6.
// ffmpeg (décode) -> égaliseur biquad (réel) -> FFT (spectre) -> QAudioSink (son)
#include <QApplication>
#include <QAudioSink>
#include <QAudioFormat>
#include <QProcess>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QListWidget>
#include <QFileDialog>
#include <QInputDialog>
#include <QTimer>
#include <QPainter>
#include <QMouseEvent>
#include <QComboBox>
#include <QCheckBox>
#include <QFileInfo>
#include <QLineEdit>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QRandomGenerator>
#include <QPainterPath>
#include <QCursor>
#include <QDirIterator>
#include <cmath>
#include <vector>
#include <algorithm>
#include <functional>
#include <cstdint>

// ---------------------------------------------------------------------------
// FFT (radix-2 Cooley–Tukey)
// ---------------------------------------------------------------------------
static void fft(std::vector<double>& re, std::vector<double>& im) {
    int n = (int)re.size();
    if (n <= 1 || (n & (n-1))) return;
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2*M_PI/len, wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double xr = 1, xi = 0;
            for (int j = 0; j < len/2; j++) {
                int a = i+j, b = i+j+len/2;
                double tr = re[b]*xr - im[b]*xi;
                double ti = re[b]*xi + im[b]*xr;
                re[b] = re[a]-tr; im[b] = im[a]-ti;
                re[a] += tr;      im[a] += ti;
                double nxr = xr*wr - xi*wi;
                xi = xr*wi + xi*wr; xr = nxr;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Biquad + Égaliseur (RBJ peaking, 10 bandes) — traitement audio réel
// ---------------------------------------------------------------------------
static const double EQ_FREQS[10] = {31,62,125,250,500,1000,2000,4000,8000,16000};

struct Biquad {
    double b0=1,b1=0,b2=0,a1=0,a2=0,z1=0,z2=0;
    void set(double B0,double B1,double B2,double A0,double A1,double A2){
        b0=B0/A0; b1=B1/A0; b2=B2/A0; a1=A1/A0; a2=A2/A0;
    }
    double proc(double x){
        double y=b0*x+z1;
        z1=b1*x-a1*y+z2;
        z2=b2*x-a2*y;
        return y;
    }
};

class Equalizer {
public:
    bool enabled=false;
    double gains[10]={0};
    double preGain=0;
    double sr;
    Biquad bands[10];
    Equalizer(int sampleRate):sr(sampleRate){ recompute(); }
    void recompute(){
        for(int b=0;b<10;b++){
            if(gains[b]==0){ bands[b]=Biquad(); continue; }
            bands[b]=peaking(EQ_FREQS[b], gains[b], 1.0);
        }
    }
    Biquad peaking(double f,double g,double Q){
        double A=pow(10,g/40.0), w0=2*M_PI*f/sr, alpha=sin(w0)/(2*Q);
        Biquad q; q.set(1+alpha*A, -2*cos(w0), 1-alpha*A, 1+alpha/A, -2*cos(w0), 1-alpha/A);
        return q;
    }
    void process(std::vector<float>& s){
        double pg = pow(10, preGain/20.0);
        if(enabled){
            for(size_t i=0;i+1<s.size();i+=2){
                double l=s[i]*pg, r=s[i+1]*pg;
                for(int b=0;b<10;b++){ l=bands[b].proc(l); r=bands[b].proc(r); }
                s[i]=l; s[i+1]=r;
            }
        } else if(preGain!=0){
            for(auto& v:s) v*=pg;
        }
    }
};

// ---------------------------------------------------------------------------
// Moteur audio : ffmpeg -> PCM -> EQ -> FIFO (drainé à vitesse réelle) -> QAudioSink ; FFT sur le son joué
// ---------------------------------------------------------------------------
class AudioEngine : public QObject {
public:
    static const int SR=48000;
    QAudioSink* sink=nullptr;
    QProcess* proc=nullptr;
    QIODevice* dev=nullptr;
    Equalizer* eq=nullptr;
    QString currentUrl;
    bool playing=false, paused=false, sourceEnded=false;
    double position=0, duration=0;
    std::vector<float> ring;
    int rpos=0;
    QByteArray fifo;
    QTimer* drainTimer=nullptr;
    std::function<void()> onEnded;

    AudioEngine(QObject* parent=nullptr):QObject(parent){
        QAudioFormat fmt;
        fmt.setSampleRate(SR);
        fmt.setChannelCount(2);
        fmt.setSampleFormat(QAudioFormat::Int16);
        sink = new QAudioSink(fmt, this);
        sink->setBufferSize(48000*2*2); // 1s de buffer
        eq = new Equalizer(SR);
        ring.resize(16384);
        drainTimer = new QTimer(this);
        connect(drainTimer, &QTimer::timeout, this, &AudioEngine::drain);
    }
    ~AudioEngine(){ stop(); }

    bool play(QString url){
        stop();
        currentUrl = url;
        duration = probeDuration(url);
        fifo.clear();
        sourceEnded = false;
        position = 0;
        proc = new QProcess(this);
        QStringList args;
        args << "-v" << "error" << "-nostdin" << "-i" << url
             << "-f" << "s16le" << "-ac" << "2" << "-ar" << QString::number(SR) << "-";
        connect(proc, &QProcess::readyReadStandardOutput, this, [this]{ onData(); });
        connect(proc, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int,QProcess::ExitStatus){ onFinished(); });
        proc->start("ffmpeg", args);
        dev = sink->start();
        sink->setVolume(0.8);
        drainTimer->start(10);
        playing=true; paused=false;
        return true;
    }

    void stop(){
        if(proc){ proc->kill(); proc->waitForFinished(200); proc->deleteLater(); proc=nullptr; }
        if(sink) sink->stop();
        drainTimer->stop();
        fifo.clear();
        dev=nullptr;
        playing=false; paused=false; sourceEnded=false; position=0;
    }

    void togglePause(){
        if(!playing) return;
        if(paused){ sink->resume(); paused=false; drainTimer->start(10); }
        else { sink->suspend(); paused=true; drainTimer->stop(); }
    }

    void seek(double sec){
        if(currentUrl.isEmpty()) return;
        if(proc){ proc->kill(); proc->waitForFinished(200); proc->deleteLater(); proc=nullptr; }
        if(sink) sink->stop();
        fifo.clear();
        sourceEnded = false;
        position = sec;
        proc = new QProcess(this);
        QStringList args;
        args << "-v" << "error" << "-nostdin" << "-ss" << QString::number(sec,'f',2)
             << "-i" << currentUrl << "-f" << "s16le" << "-ac" << "2"
             << "-ar" << QString::number(SR) << "-";
        connect(proc, &QProcess::readyReadStandardOutput, this, [this]{ onData(); });
        connect(proc, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int,QProcess::ExitStatus){ onFinished(); });
        proc->start("ffmpeg", args);
        dev = sink->start();
        drainTimer->start(10);
        playing=true; paused=false;
    }

    double probeDuration(const QString& url){
        QProcess p; p.start("ffprobe", {"-v","error","-show_entries","format=duration",
            "-of","default=noprint_wrappers=1:nokey=1", url});
        p.waitForFinished(3000);
        return QString(p.readAllStandardOutput()).trimmed().toDouble();
    }

    void setVolume(int v){ if(sink) sink->setVolume(v/100.0); }

    // ffmpeg a décodé : applique EQ et met en file d'attente (pas encore joué)
    void onData(){
        if(!proc) return;
        QByteArray d = proc->readAllStandardOutput();
        int n = d.size()/2;
        if(n<=0) return;
        std::vector<float> s(n);
        for(int i=0;i<n;i++){
            int16_t v = (int16_t)((uint8_t)d[2*i] | (uint8_t)d[2*i+1]<<8);
            s[i] = v/32768.0f;
        }
        eq->process(s);
        for(int i=0;i<n;i++){
            float v=s[i]; if(v>1)v=1; if(v<-1)v=-1;
            int16_t iv=(int16_t)(v*32767);
            d[2*i]=iv&0xff; d[2*i+1]=(iv>>8)&0xff;
        }
        fifo.append(d);
    }

    // vide la FIFO vers le sink à la cadence du périphérique (vitesse réelle)
    void drain(){
        if(!dev || paused) return;
        qint64 free = sink->bytesFree();
        if(free <= 0) return;
        qint64 toWrite = std::min<qint64>(free, fifo.size());
        if(toWrite > 0){
            const uint8_t* p = (const uint8_t*)fifo.constData();
            int ns = (int)(toWrite/2);
            for(int i=0;i+1<ns;i+=2){
                int16_t l = (int16_t)(p[2*i] | p[2*i+1]<<8);
                int16_t r = (int16_t)(p[2*i+2] | p[2*i+3]<<8);
                ring[rpos] = (l/32768.0f + r/32768.0f)*0.5f;
                rpos = (rpos+1)%(int)ring.size();
            }
            dev->write(fifo.constData(), toWrite);
            fifo.remove(0, (int)toWrite);
            position += toWrite/192000.0;
        }
        if(sourceEnded && fifo.isEmpty()){
            drainTimer->stop();
            playing=false; paused=false;
            if(onEnded) onEnded();
        }
    }

    void onFinished(){
        sourceEnded = true;
        if(proc){ proc->deleteLater(); proc=nullptr; }
    }

    // derniers échantillons mono (pour l'oscilloscope)
    void waveform(int n, std::vector<float>& out){
        out.clear();
        if(ring.empty()) return;
        for(int i=0;i<n;i++){
            int idx = (rpos - n + i); if(idx<0) idx += (int)ring.size();
            out.push_back(ring[idx]);
        }
    }

    void spectrum(int nBars, std::vector<float>& out){
        out.assign(nBars, 0.0f);
        const int N=2048;
        if((int)ring.size() < N) return;
        std::vector<double> re(N), im(N,0.0);
        for(int i=0;i<N;i++){
            int idx = (rpos - N + i);
            if(idx<0) idx += (int)ring.size();
            re[i] = ring[idx] * 0.5*(1-cos(2*M_PI*i/(N-1)));
        }
        fft(re, im);
        std::vector<double> mag(N/2);
        for(int i=0;i<N/2;i++) mag[i]=hypot(re[i],im[i]);
        double lo=20, hi=SR*0.45, binHz=SR/(double)N;
        for(int b=0;b<nBars;b++){
            double f1=lo*pow(hi/lo,(double)b/nBars);
            double f2=lo*pow(hi/lo,(double)(b+1)/nBars);
            int b1=f1/binHz, b2=f2/binHz; if(b2<=b1) b2=b1+1;
            double sum=0,mx=0; int cnt=0;
            for(int j=b1;j<b2 && j<N/2;j++){ sum+=mag[j]; mx=std::max(mx,mag[j]); cnt++; }
            if(cnt>0) out[b]=(sum/cnt)+0.5*mx;
        }
        double s=0; for(auto& v:out) s+=v; s/=nBars; if(s<1e-6) s=1e-6;
        for(auto& v:out){ v/=(s*3); if(v>1)v=1; }
    }
};

// ---------------------------------------------------------------------------
// Tête de robot (pixel art, extrait du screenshot NightCafe)
// ---------------------------------------------------------------------------
static const char* ROBOT[] = {
    "                    ####",
    "                   ##  ##",
    "                   ##  ##",
    "                   #####",
    "                    ###",
    "                     ##",
    "                     ##",
    "                    ####",
    "                ############",
    "           #######################",
    "  #       #########################      #",
    "  ##     ##     ############      ##     ##",
    "  ##    ##      #############      #     #",
    "  ##   ## ######################## ##    #",
    "  ##   # ########################## ##   #",
    "  ##  ################################   ##",
    "  ##  ######  #####       ####  ######   #",
    "  ##   ####  #######    #######  #####   #",
    "  ##  ##### ##    ##    ##    ## #####  ###",
    " ########## #  ##  ##  ##  ##  # ##########",
    " ########## ## ## ###  ### ## ## ######## #",
    "########### ## ## ###  ### ## ## ###########",
    "#### ###### ## ## ##    ## ## ## ###### ####",
    "###########  #######    #######  ###########",
    "###########  ###### #### ######  ###########",
    "#### ######         ####         ###### ####",
    "#### ######         ####         ###### ####",
    " ##########         ####         ##########",
    "  #########                      ##########",
    "  #########  ##################  ##########",
    "       #### ###################  #####",
    "      ##### ###################  #####",
    "      ##### #################### #####",
    "      ###### ################## ######",
    "      ## ########################## ##",
    "       ## ######################## ##",
    "        #############################",
    "         ##########################",
    "                # ######## #",
    "                # ######## #",
    "        ###     # ######## #     ####",
    "     # ########## ######## ########## ##",
    "    ############# ######## ######## #####",
    "   ############################### ######",
    "  #### ##############################  ###",
    " ### ##  ##########################  ## ###",
    "############################################",
    "#############################################",
    "## #  #######  ############## #####  #   ####",
};
static const int ROBOT_W = 45, ROBOT_H = 49;

// ---------------------------------------------------------------------------
// Visualiseur : spectre + peak-hold façon Winamp
// ---------------------------------------------------------------------------
class SpectrumWidget : public QWidget {
public:
    AudioEngine* eng=nullptr;
    std::vector<float> bars, smooth, peaks;
    int mode=0; // 0 barres, 1 points, 2 onde, 3 cristal

    SpectrumWidget(AudioEngine* e, QWidget* p=nullptr):QWidget(p),eng(e){ setMinimumHeight(120); }

    void step(){
        if(eng) eng->spectrum(64, bars);
        if(smooth.size()!=bars.size()){ smooth.assign(bars.size(),0); peaks.assign(bars.size(),0); }
        for(size_t i=0;i<bars.size();i++){
            if(bars[i]>smooth[i]) smooth[i]+=(bars[i]-smooth[i])*0.6f;
            else smooth[i]+=(bars[i]-smooth[i])*0.2f;
            peaks[i]=std::max(peaks[i]*0.93f, smooth[i]);
        }
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#0b0b11"));
        if(mode==2){ paintWave(p); return; }
        if(mode==3){ paintCrystal(p); return; }
        if(smooth.empty()) step();
        int n=(int)smooth.size(); if(n==0) return;
        int W=width(), H=height();
        float bw = (float)W/n;
        QColor cLow("#00ff41"), cMid("#00d5ff"), cHigh("#ff007f");
        for(int i=0;i<n;i++){
            float lvl=smooth[i], peak=peaks[i];
            int x=(int)(i*bw);
            QColor c;
            if(lvl<0.34f){ c=lerp(cLow,cMid,lvl/0.34f); }
            else { float t=(lvl-0.34f)/0.66f; c=lerp(cMid,cHigh,t); }
            if(mode==0){ // barres pleines
                int h=(int)(lvl*(H-8));
                p.fillRect(x, H-h-4, (int)bw-1, h, c);
                int py=H-(int)(peak*(H-8))-5;
                p.fillRect(x, py, (int)bw-1, 2, QColor("#ffffff"));
            } else { // mode 1 : points (pastille au niveau courant)
                int y=H-(int)(lvl*(H-8))-6;
                p.fillRect(x, y, (int)bw-1, 3, c);
            }
        }
    }

    void paintWave(QPainter& p){
        if(!eng) return;
        std::vector<float> w;
        eng->waveform(width(), w);
        if(w.size()<2) return;
        int H=height();
        QPainterPath path;
        float midy=H/2.0f;
        for(size_t i=0;i<w.size();i++){
            float x=(float)i/(w.size()-1)*width();
            float y=midy - w[i]*(H*0.45f);
            if(i==0) path.moveTo(x,y); else path.lineTo(x,y);
        }
        p.setPen(QPen(QColor("#00d5ff"), 2));
        p.drawPath(path);
        // ligne centrale
        p.setPen(QColor("#262632"));
        p.drawLine(0, H/2, width(), H/2);
    }

    // Tête de robot centrale (blanc pur) + oscillation "vocale" gauche/droite
    void paintCrystal(QPainter& p){
        if(!eng) return;
        int W=width(), H=height();
        int cx=W/2, cy=H/2;

        // robot (pixel art) centré, ~60% de la hauteur
        int cell = H*0.56 / ROBOT_H;
        if(cell < 2) cell = 2;
        int rw = ROBOT_W*cell, rh = ROBOT_H*cell;
        int rx = cx - rw/2, ry = cy - rh/2;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#ffffff"));
        for(int y=0;y<ROBOT_H;y++){
            const char* row = ROBOT[y];
            for(int x=0; row[x]; x++){
                if(row[x]=='#') p.fillRect(rx+x*cell, ry+y*cell, cell, cell, QColor("#ffffff"));
            }
        }

        // oscillation vocale : forme d'onde brute à gauche et à droite
        std::vector<float> w;
        eng->waveform(W, w);
        if(w.size()<2) return;
        int margin = 14;
        int leftW = rx - margin;
        if(leftW < 0) leftW = 0;
        int rightStart = rx + rw + margin;
        float amp = H*0.26f;

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor("#ffffff"), 2));
        QPainterPath lp;
        for(size_t i=0;i<w.size();i++){
            float t=(float)i/(w.size()-1);
            float x=t*leftW;
            float y=cy + w[i]*amp;
            if(i==0) lp.moveTo(x,y); else lp.lineTo(x,y);
        }
        p.drawPath(lp);

        QPainterPath rp;
        for(size_t i=0;i<w.size();i++){
            float t=(float)i/(w.size()-1);
            float x=rightStart + t*(W-rightStart-margin);
            float y=cy + w[i]*amp;
            if(i==0) rp.moveTo(x,y); else rp.lineTo(x,y);
        }
        p.drawPath(rp);
    }

    static QColor lerp(QColor a, QColor b, float t){
        t=std::max(0.0f,std::min(1.0f,t));
        return QColor(a.red()+(b.red()-a.red())*t,
                      a.green()+(b.green()-a.green())*t,
                      a.blue()+(b.blue()-a.blue())*t);
    }
};

// ---------------------------------------------------------------------------
// Fenêtre principale (frameless, skin sombre néon)
// ---------------------------------------------------------------------------
class MainWindow : public QWidget {
public:
    AudioEngine* eng=nullptr;
    SpectrumWidget* spec=nullptr;
    QListWidget* playlist=nullptr;
    QSlider *volSlider=nullptr, *seekSlider=nullptr, *eqSliders[10];
    QLabel *titleLabel=nullptr, *timeLabel=nullptr;
    QComboBox* presetCombo=nullptr;
    QLineEdit* searchBox=nullptr;
    QPushButton *shuffleBtn=nullptr, *loopBtn=nullptr, *visBtn=nullptr;
    QStringList queue;
    bool seekUpdating=false;
    bool shuffle=false;
    int loop=0; // 0=off, 1=all, 2=one
    QPoint dragPos;

    MainWindow(){
        setWindowFlags(Qt::FramelessWindowHint);
        setMinimumSize(880, 600);
        setStyleSheet(style());
        build();
        loadRadios(); // crée le fichier radios.txt au premier lancement

        QTimer* t = new QTimer(this);
        connect(t,&QTimer::timeout,this,[this]{
            spec->step(); spec->update();
            updateTime();
        });
        t->start(33);
    }

    void build(){
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0,0,0,0); root->setSpacing(0);

        // Barre de titre
        auto* titleBar = new QWidget(this); titleBar->setObjectName("titlebar");
        auto* tb = new QHBoxLayout(titleBar); tb->setContentsMargins(12,0,8,0);
        auto* name = new QLabel(" ZERO·KRISTAL "); name->setObjectName("brand");
        auto* mini = new QPushButton("—"); mini->setObjectName("tb");
        auto* close = new QPushButton("✕"); close->setObjectName("tb");
        tb->addWidget(name); tb->addStretch(); tb->addWidget(mini); tb->addWidget(close);
        connect(mini,&QPushButton::clicked,this,&QWidget::showMinimized);
        connect(close,&QPushButton::clicked,this,&QWidget::close);
        root->addWidget(titleBar);

        // Recherche YouTube
        auto* searchRow = new QHBoxLayout(); searchRow->setContentsMargins(8,3,8,3);
        searchBox = new QLineEdit(); searchBox->setObjectName("search");
        searchBox->setPlaceholderText("Recherche YouTube… (Entrée)");
        searchRow->addWidget(searchBox);
        root->addLayout(searchRow);
        connect(searchBox,&QLineEdit::returnPressed,this,[this]{
            searchYouTube(searchBox->text());
        });

        // Visualiseur
        spec = new SpectrumWidget(eng, this);
        root->addWidget(spec, 3);

        // Piste + temps
        auto* info = new QHBoxLayout();
        titleLabel = new QLabel("Aucune piste"); titleLabel->setObjectName("title");
        timeLabel = new QLabel("--:-- / --:--"); timeLabel->setObjectName("time");
        info->addWidget(titleLabel); info->addStretch(); info->addWidget(timeLabel);
        root->addLayout(info);

        // Seek
        seekSlider = new QSlider(Qt::Horizontal); seekSlider->setRange(0,1000);
        seekSlider->setObjectName("seek");
        connect(seekSlider,&QSlider::sliderReleased,this,[this]{
            if(eng && eng->duration>0) eng->seek(seekSlider->value());
        });
        root->addWidget(seekSlider);

        // Contrôles
        auto* ctrl = new QHBoxLayout(); ctrl->setContentsMargins(8,4,8,4);
        auto* prev=new QPushButton("⏮︎");      prev->setObjectName("prev");
        auto* play=new QPushButton("▶");      play->setObjectName("play");
        auto* stop=new QPushButton("⏹︎");      stop->setObjectName("stopbtn");
        auto* next=new QPushButton("⏭︎︎");      next->setObjectName("next");
        shuffleBtn=new QPushButton("🔀");       shuffleBtn->setObjectName("toggle");
        shuffleBtn->setCheckable(true); shuffleBtn->setChecked(false);
        shuffleBtn->setToolTip("Lecture aléatoire");
        loopBtn=new QPushButton("🔁");         loopBtn->setObjectName("toggle");
        loopBtn->setCheckable(true); loopBtn->setChecked(false);
        loopBtn->setToolTip("Boucle (off / toutes / une)");
        visBtn=new QPushButton("🎨 Barres");   visBtn->setObjectName("ctrl");
        auto* open=new QPushButton("📂 Ouvrir"); open->setObjectName("ctrl");
        auto* openMenu = new QMenu(this);
        openMenu->addAction("📂 Ouvrir des fichiers…", this, [this]{ openFiles(); });
        openMenu->addAction("📁 Ouvrir un dossier…", this, [this]{ openFolder(); });
        open->setMenu(openMenu);
        auto* urlB=new QPushButton("🔗 URL");   urlB->setObjectName("ctrl");
        auto* radio=new QPushButton("📻 Radio");radio->setObjectName("ctrl");
        auto* addRadio=new QPushButton("➕");    addRadio->setObjectName("ctrl");
        addRadio->setToolTip("Ajouter une web radio");
        auto* plBtn=new QPushButton("💾 Playlists"); plBtn->setObjectName("ctrl");

        ctrl->addWidget(prev); ctrl->addWidget(play); ctrl->addWidget(stop); ctrl->addWidget(next);
        ctrl->addSpacing(6); ctrl->addWidget(shuffleBtn); ctrl->addWidget(loopBtn);
        ctrl->addSpacing(12); ctrl->addWidget(open); ctrl->addWidget(urlB); ctrl->addWidget(radio);
        ctrl->addWidget(addRadio); ctrl->addWidget(visBtn); ctrl->addWidget(plBtn);
        ctrl->addStretch();
        volSlider=new QSlider(Qt::Horizontal); volSlider->setRange(0,100); volSlider->setValue(80);
        volSlider->setFixedWidth(120);
        ctrl->addWidget(new QLabel("🔊")); ctrl->addWidget(volSlider);
        root->addLayout(ctrl);

        connect(play,&QPushButton::clicked,this,[this]{ if(eng) eng->togglePause(); });
        connect(stop,&QPushButton::clicked,this,[this]{ if(eng) eng->stop(); });
        connect(prev,&QPushButton::clicked,this,[this]{ stepTrack(-1); });
        connect(next,&QPushButton::clicked,this,[this]{ stepTrack(+1); });
        connect(urlB,&QPushButton::clicked,this,&MainWindow::openURL);
        connect(radio,&QPushButton::clicked,this,&MainWindow::playRadio);
        connect(addRadio,&QPushButton::clicked,this,&MainWindow::addRadioDialog);
        connect(visBtn,&QPushButton::clicked,this,[this]{ cycleVisualizer(); });
        connect(plBtn,&QPushButton::clicked,this,&MainWindow::playlistMenu);
        connect(shuffleBtn,&QPushButton::toggled,this,[this](bool on){ shuffle=on; });
        connect(loopBtn,&QPushButton::clicked,this,[this]{ cycleLoop(); });
        connect(volSlider,&QSlider::valueChanged,this,[this](int v){ if(eng) eng->setVolume(v); });
        auto* bot = new QHBoxLayout(); bot->setContentsMargins(0,0,0,0);
        playlist = new QListWidget(); playlist->setObjectName("pl");
        connect(playlist,&QListWidget::itemDoubleClicked,this,[this](QListWidgetItem* it){
            int i=playlist->row(it); if(i>=0 && i<queue.size()) playIndex(i);
        });
        bot->addWidget(playlist, 1);

        auto* eqPanel=new QWidget(); eqPanel->setFixedWidth(250); eqPanel->setObjectName("eq");
        auto* eqL=new QVBoxLayout(eqPanel);
        auto* eqHead=new QLabel(" ÉGALISEUR"); eqHead->setObjectName("brand");
        eqL->addWidget(eqHead);
        presetCombo=new QComboBox();
        presetCombo->addItems({"Flat","Bass Boost","Treble Boost","V-Shape","Rock","Pop","Jazz","Electronic","Loudness","8-Bit Retro"});
        eqL->addWidget(presetCombo);
        auto* sl=new QHBoxLayout();
        for(int i=0;i<10;i++){
            auto* col=new QVBoxLayout();
            auto* s=new QSlider(Qt::Vertical); s->setRange(-24,24); s->setValue(0); s->setObjectName("eqslider");
            eqSliders[i]=s;
            col->addWidget(s);
            auto* lab=new QLabel(EQ_FREQS[i]>=1000 ? QString::number(EQ_FREQS[i]/1000)+"k" : QString::number((int)EQ_FREQS[i]));
            lab->setAlignment(Qt::AlignCenter);
            col->addWidget(lab);
            sl->addLayout(col);
        }
        eqL->addLayout(sl);
        auto* eqOn=new QCheckBox("Activer l'égaliseur"); eqOn->setChecked(false);
        eqL->addWidget(eqOn);
        connect(eqOn,&QCheckBox::toggled,this,[this](bool on){ if(eng){eng->eq->enabled=on; eng->eq->recompute();} });
        connect(presetCombo,&QComboBox::currentTextChanged,this,&MainWindow::applyPreset);
        for(int i=0;i<10;i++){
            connect(eqSliders[i],&QSlider::valueChanged,this,[this,i](int v){
                if(eng){ eng->eq->gains[i]=v; eng->eq->recompute(); }
            });
        }
        bot->addWidget(eqPanel);
        root->addLayout(bot, 2);
    }

    void searchYouTube(const QString& query){
        if(query.trimmed().isEmpty()) return;
        QProcess yt;
        yt.start("yt-dlp", {"--flat-playlist","--dump-json","--no-warnings","--no-playlist",
                           "ytsearch15:"+query});
        yt.waitForFinished(20000);
        QByteArray out = yt.readAllStandardOutput();
        playlist->clear();
        queue.clear();
        for(auto& line : out.split('\n')){
            if(line.trimmed().isEmpty()) continue;
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(line, &err);
            if(err.error != QJsonParseError::NoError) continue;
            QJsonObject o = doc.object();
            QString id = o["id"].toString();
            QString title = o["title"].toString();
            QString channel = o["channel"].toString();
            if(id.isEmpty()) continue;
            queue << QString("https://www.youtube.com/watch?v=")+id;
            playlist->addItem(title + "  —  " + channel);
        }
        if(queue.isEmpty()) playlist->addItem("Aucun résultat");
    }

    void applyPreset(const QString& name){
        static const double P[10][10] = {
            {0,0,0,0,0,0,0,0,0,0},
            {6,4,2,1,0,0,0,0,0,0},
            {0,0,0,0,0,0,1,2,4,6},
            {4,3,2,1,0,0,1,2,3,4},
            {4,2,0,-1,-2,0,1,2,3,2},
            {-1,1,2,3,1,-1,-1,0,1,1},
            {3,2,1,2,-1,-1,0,1,2,3},
            {4,3,1,0,-1,0,1,1,3,4},
            {5,4,2,0,0,-1,0,1,2,3},
            {3,2,1,0,-1,0,1,2,3,4},
        };
        QStringList names = {"Flat","Bass Boost","Treble Boost","V-Shape","Rock","Pop","Jazz","Electronic","Loudness","8-Bit Retro"};
        int idx = names.indexOf(name);
        if(idx<0||idx>=10) return;
        for(int i=0;i<10;i++) eqSliders[i]->setValue((int)P[idx][i]);
    }

    void openFiles(){
        QStringList files = QFileDialog::getOpenFileNames(this,"Ouvrir des fichiers audio", QString(),
            "Audio (*.mp3 *.flac *.wav *.ogg *.m4a *.aac *.opus)");
        for(auto& f: files){ queue<<f; playlist->addItem(QFileInfo(f).fileName()); }
        if(!files.isEmpty() && !eng->playing) playIndex(queue.size()-files.size());
    }

    void openFolder(){
        QString dir = QFileDialog::getExistingDirectory(this, "Ouvrir un dossier", QString());
        if(dir.isEmpty()) return;
        static const QStringList exts = {"mp3","flac","wav","ogg","m4a","aac","opus","aiff","aif","ape"};
        QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
        int added = 0;
        while(it.hasNext()){
            it.next();
            QFileInfo fi = it.fileInfo();
            if(exts.contains(fi.suffix().toLower())){
                queue << fi.absoluteFilePath();
                playlist->addItem(fi.fileName());
                added++;
            }
        }
        if(added>0 && !eng->playing) playIndex(queue.size()-added);
    }

    void openURL(){
        bool ok=false;
        QString u=QInputDialog::getText(this,"Ouvrir une URL/stream","URL (web radio, YouTube, flux) :",QLineEdit::Normal,"",&ok);
        if(ok && !u.isEmpty()){ queue<<u; playlist->addItem(u); playIndex(queue.size()-1); }
    }

    // --- Config (radios + playlists) ---
    QString cfgDir(){
        QString d = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/zer0-kristal";
        QDir().mkpath(d); return d;
    }
    QString radioFile(){ return cfgDir()+"/radios.txt"; }
    QString plDir(){ QString d=cfgDir()+"/playlists"; QDir().mkpath(d); return d; }

    QStringList loadRadios(){
        QStringList defs = {
            "SomaFM Groove Salad|https://ice1.somafm.com/groovesalad-256-mp3",
            "SomaFM Drone Zone|https://ice1.somafm.com/dronezone-256-mp3",
            "Radio Paradise|https://stream.radioparadise.com/mp3-192",
            "FIP|https://icecast.radiofrance.fr/fip-midfi.mp3",
            "KEXP|https://kexp-mp3-128.streamguys1.com/kexp128.mp3",
            "NTS 1|https://stream.nts.live/nts1",
            "TSF Jazz|https://tsfjazz.ice.infomaniak.ch/tsfjazz-128.mp3",
        };
        QFile f(radioFile());
        if(!f.exists()){
            QFile w(radioFile()); if(w.open(QIODevice::WriteOnly)){ for(auto&d:defs) w.write((d+"\n").toUtf8()); }
            return defs;
        }
        if(!f.open(QIODevice::ReadOnly)) return defs;
        QStringList out;
        while(!f.atEnd()){ QString l=QString::fromUtf8(f.readLine()).trimmed(); if(!l.isEmpty()) out<<l; }
        return out.isEmpty()?defs:out;
    }

    void addRadioDialog(){
        bool ok=false;
        QString name = QInputDialog::getText(this,"Ajouter une radio","Nom :",QLineEdit::Normal,"",&ok);
        if(!ok) return;
        QString url = QInputDialog::getText(this,"Ajouter une radio","URL du flux (direct .mp3/.aac) :",QLineEdit::Normal,"",&ok);
        if(!ok || url.isEmpty()) return;
        QFile f(radioFile()); if(f.open(QIODevice::Append)){ f.write((name+"|"+url+"\n").toUtf8()); }
    }

    void playlistMenu(){
        QMenu menu(this);
        menu.addAction("💾 Sauvegarder la file", this, [this]{ savePlaylistDialog(); });
        menu.addAction("➕ Ajouter la piste courante à…", this, [this]{ addToPlaylistDialog(); });
        menu.addSeparator();
        // charger
        QMenu* loadM = menu.addMenu("📂 Charger");
        for(auto& n : listPlaylists()) loadM->addAction(n, this, [this,n]{ loadPlaylist(n); });
        QMenu* delM = menu.addMenu("🗑 Supprimer");
        for(auto& n : listPlaylists()) delM->addAction(n, this, [this,n]{ deletePlaylist(n); });
        menu.exec(QCursor::pos());
    }

    QStringList listPlaylists(){
        QDir d(plDir());
        QStringList out;
        for(auto& e : d.entryList(QStringList()<<"*.txt", QDir::Files)) out << e.section('.',0,0);
        return out;
    }

    void savePlaylistDialog(){
        bool ok=false;
        QString n = QInputDialog::getText(this,"Nouvelle playlist","Nom :",QLineEdit::Normal,"",&ok);
        if(ok && !n.isEmpty()) savePlaylist(n);
    }
    void savePlaylist(const QString& n){
        QFile w(plDir()+"/"+n+".txt"); if(w.open(QIODevice::WriteOnly)){
            for(auto& u:queue) w.write((u+"\n").toUtf8());
        }
    }
    void loadPlaylist(const QString& n){
        QFile f(plDir()+"/"+n+".txt"); if(!f.open(QIODevice::ReadOnly)) return;
        playlist->clear(); queue.clear();
        while(!f.atEnd()){ QString u=QString::fromUtf8(f.readLine()).trimmed(); if(!u.isEmpty()){ queue<<u; playlist->addItem(QFileInfo(u).fileName()); } }
    }
    void deletePlaylist(const QString& n){ QFile::remove(plDir()+"/"+n+".txt"); }

    void addToPlaylistDialog(){
        QStringList pl = listPlaylists();
        if(pl.isEmpty()) return;
        bool ok=false;
        QString n = QInputDialog::getItem(this,"Ajouter à…","Playlist :",pl,0,false,&ok);
        if(!ok) return;
        int i = playlist->currentRow();
        if(i<0 || i>=queue.size()) return;
        QFile f(plDir()+"/"+n+".txt"); if(f.open(QIODevice::Append)) f.write((queue[i]+"\n").toUtf8());
    }

    void cycleVisualizer(){
        spec->mode = (spec->mode+1)%4;
        visBtn->setText(spec->mode==0?"🎨 Barres":spec->mode==1?"🎨 Points":spec->mode==2?"🎨 Onde":"🎨 Cristal");
    }

    void cycleLoop(){
        loop = (loop+1)%3;
        loopBtn->setChecked(loop!=0);
        if(loop==0){ loopBtn->setText("🔁"); loopBtn->setToolTip("Boucle : off"); }
        else if(loop==1){ loopBtn->setText("🔁"); loopBtn->setToolTip("Boucle : toutes"); }
        else { loopBtn->setText("🔂"); loopBtn->setToolTip("Boucle : une"); }
    }

    // piste suivante automatique (à la fin d'une piste)
    void autoNext(){
        if(queue.isEmpty()) return;
        int cur = playlist->currentRow();
        int nxt;
        if(loop==2){ nxt = cur; }
        else if(shuffle && queue.size()>1){
            nxt = QRandomGenerator::global()->bounded(queue.size());
            if(nxt==cur) nxt=(cur+1)%queue.size();
        }
        else {
            nxt = cur+1;
            if(nxt>=queue.size()){ if(loop==1) nxt=0; else return; }
        }
        playlist->setCurrentRow(nxt);
        playIndex(nxt);
    }

    void playRadio(){
        QStringList presets = loadRadios();
        QStringList names; for(auto& s:presets) names<<s.section('|',0,0);
        bool ok=false;
        QString c=QInputDialog::getItem(this,"Web Radio","Station :",names,0,false,&ok);
        if(ok){ for(auto& s:presets) if(s.startsWith(c+"|")){ playSource(c, s.section('|',1)); break; } }
    }

    void playSource(const QString& name, const QString& url){
        queue<<url; playlist->addItem(name); playIndex(queue.size()-1);
    }

    void playIndex(int i){
        if(!eng || i<0 || i>=queue.size()) return;
        QString url=queue[i];
        if(url.contains("youtube.com")||url.contains("youtu.be")){
            QProcess yt; yt.start("yt-dlp",{"-f","bestaudio","--get-url",url});
            yt.waitForFinished(15000);
            QString r=QString(yt.readAllStandardOutput()).trimmed();
            if(!r.isEmpty()) url=r;
        }
        eng->play(url);
        if(playlist->item(i)) titleLabel->setText(playlist->item(i)->text());
        if(playlist->item(i)) playlist->setCurrentRow(i);
        seekSlider->setRange(0,(int)eng->duration);
    }

    void stepTrack(int d){
        if(queue.isEmpty()) return;
        int cur = playlist->currentRow();
        int nxt = (cur + d + queue.size()) % queue.size();
        playlist->setCurrentRow(nxt);
        playIndex(nxt);
    }

    QString timeStr(double s){
        int t=(int)s; if(t<0)t=0;
        if(t>=3600) return QString("%1:%2:%3").arg(t/3600).arg((t%3600)/60,2,10,QChar('0')).arg(t%60,2,10,QChar('0'));
        return QString("%1:%2").arg(t/60).arg(t%60,2,10,QChar('0'));
    }

    void updateTime(){
        if(!eng) return;
        timeLabel->setText(timeStr(eng->position)+" / "+timeStr(eng->duration));
        if(eng->duration>0 && !seekUpdating){
            seekUpdating=true;
            seekSlider->setRange(0,(int)eng->duration);
            seekSlider->setValue((int)eng->position);
            seekUpdating=false;
        }
    }

    void mousePressEvent(QMouseEvent* e){
        if(e->position().y()<40) dragPos = e->globalPosition().toPoint() - frameGeometry().topLeft();
    }
    void mouseMoveEvent(QMouseEvent* e){
        if((e->buttons() & Qt::LeftButton) && !dragPos.isNull())
            move(e->globalPosition().toPoint() - dragPos);
    }

    QString style(){
        return R"(
            QWidget { background:#12121a; color:#cdd6f4; font-family:'DejaVu Sans'; font-size:13px; }
            #titlebar { background:#1a1a26; }
            #brand { color:#00d5ff; font-weight:bold; font-size:14px; background:transparent; }
            QLabel { background:transparent; }
            #title { color:#e8e8f0; font-size:15px; font-weight:bold; }
            #time { color:#7f849c; }
            QPushButton { background:#262632; border:1px solid #33333f; color:#cdd6f4; border-radius:4px; padding:4px 10px; }
            QPushButton:hover { background:#33333f; }
            #play { background:#00d5ff; color:#0d0d14; font-weight:bold; }
            #prev { background:#a855f7; color:#0d0d14; font-weight:bold; }
            #stopbtn { background:#ff4444; color:#0d0d14; font-weight:bold; }
            #next { background:#22c55e; color:#0d0d14; font-weight:bold; }
            #toggle { background:#262632; border:1px solid #33333f; color:#cdd6f4; border-radius:4px; padding:4px 8px; }
            #toggle:checked { background:#00d5ff; color:#0d0d14; border-color:#00d5ff; }
            #tb { background:transparent; border:none; padding:2px 8px; }
            #tb:hover { background:#ff4444; }
            #search { background:#0f0f16; border:1px solid #33333f; border-radius:4px; padding:6px 8px; color:#cdd6f4; }
            #search:focus { border:1px solid #00d5ff; }
            #seek { background:transparent; }
            QListWidget { background:#0f0f16; border:1px solid #262632; padding:4px; }
            QListWidget::item { padding:4px; }
            QListWidget::item:selected { background:#3a3a4a; color:#00d5ff; }
            QSlider::groove:horizontal { height:6px; background:#262632; border-radius:3px; }
            QSlider::handle:horizontal { width:14px; margin:-4px 0; background:#00d5ff; border-radius:7px; }
            QSlider::groove:vertical { width:6px; background:#262632; border-radius:3px; }
            QSlider::handle:vertical { height:14px; margin:0 -4px; background:#00ff41; border-radius:7px; }
            QComboBox { background:#262632; border:1px solid #33333f; padding:4px; }
            QCheckBox { color:#7f849c; }
            #eq { background:#0f0f16; border-left:1px solid #262632; }
            QMenu { background:#1a1a26; color:#cdd6f4; border:1px solid #33333f; }
            QMenu::item { padding:6px 20px; }
            QMenu::item:selected { background:#00d5ff; color:#0d0d14; }
        )";
    }
};

int main(int argc, char** argv){
    QApplication app(argc, argv);
    app.setApplicationName("ZERO·KRISTAL");
    AudioEngine eng;
    MainWindow w;
    w.eng = &eng;
    w.spec->eng = &eng;
    eng.onEnded = [&w]{ w.autoNext(); };
    w.show();
    if(argc > 1){
        QString path = QString::fromLocal8Bit(argv[1]);
        w.queue << path;
        w.playlist->addItem(QFileInfo(path).fileName());
        w.playIndex(w.queue.size()-1);
    }
    return app.exec();
}
