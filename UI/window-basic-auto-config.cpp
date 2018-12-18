#include "window-basic-auto-config.hpp"
#include "window-basic-main.hpp"
#include "qt-wrappers.hpp"
#include "obs-app.hpp"

#include <QMessageBox>
#include <QScreen>

#include <obs.hpp>

#include <algorithm>

#include "ui_AutoConfigStartPage.h"
#include "ui_AutoConfigVideoPage.h"
#include "ui_AutoConfigStreamPage.h"

#define wiz reinterpret_cast<AutoConfig*>(wizard())

/* ------------------------------------------------------------------------- */

#define SERVICE_PATH "service.json"

static OBSData OpenServiceSettings(std::string &type)
{
	char serviceJsonPath[512];
	int ret = GetProfilePath(serviceJsonPath, sizeof(serviceJsonPath),
			SERVICE_PATH);
	if (ret <= 0)
		return OBSData();

	OBSData data = obs_data_create_from_json_file_safe(serviceJsonPath,
			"bak");
	obs_data_release(data);

	obs_data_set_default_string(data, "type", "rtmp_common");
	type = obs_data_get_string(data, "type");

	OBSData settings = obs_data_get_obj(data, "settings");
	obs_data_release(settings);

	return settings;
}

static void GetServiceInfo(std::string &type, std::string &service,
		std::string &server, std::string &key)
{
	OBSData settings = OpenServiceSettings(type);

	service = obs_data_get_string(settings, "service");
	server = obs_data_get_string(settings, "server");
	key = obs_data_get_string(settings, "key");
}

static bool serviceSpecifiedFPS(obs_data_t *settings, int *fps_num,
	int *fps_den, bool *prefer_high_fps)
{
	obs_data_t *videoSettings = obs_data_get_obj(settings, "output_settings");
	int found_requirement = 0;
	if (videoSettings) {
		obs_data_item_t *item;
		for (item = obs_data_first(videoSettings); item; obs_data_item_next(&item)) {
			obs_data_type item_type = obs_data_item_gettype(item);
			const char *name = obs_data_item_get_name(item);

			if (!obs_data_item_has_user_value(item))
				continue;

			if (strcmp(name, "output_fps_num") == 0) {
				*fps_num = obs_data_item_get_int(item);
				found_requirement |= 0x1;
			} else if (strcmp(name, "output_fps_den") == 0) {
				*fps_den = obs_data_item_get_int(item);
				found_requirement |= 0x2;
			} else if (strcmp(name, "output_prefer_high_fps") == 0) {
				*prefer_high_fps = obs_data_item_get_bool(item);
				found_requirement |= 0x4;
			}
		}

	}
	obs_data_release(videoSettings);
	return found_requirement & 0x7;
}

/* ------------------------------------------------------------------------- */

AutoConfigStartPage::AutoConfigStartPage(QWidget *parent)
	: QWizardPage (parent),
	  ui          (new Ui_AutoConfigStartPage)
{
	ui->setupUi(this);
	setTitle(QTStr("Basic.AutoConfig.StartPage"));
	setSubTitle(QTStr("Basic.AutoConfig.StartPage.SubTitle"));
}

AutoConfigStartPage::~AutoConfigStartPage()
{
	delete ui;
}

int AutoConfigStartPage::nextId() const
{
return wiz->type == AutoConfig::Type::Recording
              ? AutoConfig::VideoPage
              : AutoConfig::StreamPage;
}

void AutoConfigStartPage::on_prioritizeStreaming_clicked()
{
	wiz->type = AutoConfig::Type::Streaming;
}

void AutoConfigStartPage::on_prioritizeRecording_clicked()
{
	wiz->type = AutoConfig::Type::Recording;
}

/* ------------------------------------------------------------------------- */

#define RES_TEXT(x)             "Basic.AutoConfig.VideoPage." x
#define RES_USE_CURRENT         RES_TEXT("BaseResolution.UseCurrent")
#define RES_USE_DISPLAY         RES_TEXT("BaseResolution.Display")
#define FPS_USE_CURRENT         RES_TEXT("FPS.UseCurrent")
#define FPS_PREFER_HIGH_FPS     RES_TEXT("FPS.PreferHighFPS")
#define FPS_PREFER_HIGH_RES     RES_TEXT("FPS.PreferHighRes")

void AutoConfigVideoPage::SettingsChanged()
{
	if (wiz->serviceSpecifiedFPS) {
		long double fpsVal =
			(long double)wiz->specificFPSNum / (long double)wiz->specificFPSDen;

		QString fpsStr = (wiz->specificFPSDen > 1)
			? QString::number(fpsVal, 'f', 2)
			: QString::number(fpsVal, 'g', 2);
		int idx = ui->fps->findData((int)AutoConfig::FPSType::ServiceSpecified);
		if (idx >= 0) {
			ui->fps->setItemText(idx, fpsStr);
		} else {
			ui->fps->addItem(fpsStr, (int)AutoConfig::FPSType::ServiceSpecified);
			ui->fps->setCurrentIndex(ui->fps->count()-1);
		}
		ui->fps->setDisabled(true);
	} else {
		int idx = ui->fps->findData((int)AutoConfig::FPSType::ServiceSpecified);
		if (idx >= 0)
			ui->fps->removeItem(idx);
		ui->fps->setCurrentIndex(0);
		ui->fps->setDisabled(false);
	}
}

void AutoConfigVideoPage::initializePage()
{
	SettingsChanged();
}

AutoConfigVideoPage::AutoConfigVideoPage(QWidget *parent)
	: QWizardPage (parent),
	  ui          (new Ui_AutoConfigVideoPage)
{
	ui->setupUi(this);

	setTitle(QTStr("Basic.AutoConfig.VideoPage"));
	setSubTitle(QTStr("Basic.AutoConfig.VideoPage.SubTitle"));

	obs_video_info ovi;
	obs_get_video_info(&ovi);

	long double fpsVal =
		(long double)ovi.fps_num / (long double)ovi.fps_den;

	QString fpsStr = (ovi.fps_den > 1)
		? QString::number(fpsVal, 'f', 2)
		: QString::number(fpsVal, 'g', 2);

	ui->fps->addItem(QTStr(FPS_PREFER_HIGH_FPS),
			(int)AutoConfig::FPSType::PreferHighFPS);
	ui->fps->addItem(QTStr(FPS_PREFER_HIGH_RES),
			(int)AutoConfig::FPSType::PreferHighRes);
	ui->fps->addItem(QTStr(FPS_USE_CURRENT).arg(fpsStr),
			(int)AutoConfig::FPSType::UseCurrent);
	ui->fps->addItem(QStringLiteral("30"), (int)AutoConfig::FPSType::fps30);
	ui->fps->addItem(QStringLiteral("60"), (int)AutoConfig::FPSType::fps60);
	ui->fps->setCurrentIndex(0);

	QString cxStr = QString::number(ovi.base_width);
	QString cyStr = QString::number(ovi.base_height);

	int encRes = int(ovi.base_width << 16) | int(ovi.base_height);
	ui->canvasRes->addItem(QTStr(RES_USE_CURRENT).arg(cxStr, cyStr),
			(int)encRes);

	QList<QScreen*> screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); i++) {
		QScreen *screen = screens[i];
		QSize as = screen->size();

		encRes = int(as.width() << 16) | int(as.height());

		QString str = QTStr(RES_USE_DISPLAY)
			.arg(QString::number(i + 1),
			     QString::number(as.width()),
			     QString::number(as.height()));

		ui->canvasRes->addItem(str, encRes);
	}

	auto addRes = [&] (int cx, int cy)
	{
		encRes = (cx << 16) | cy;
		QString str = QString("%1x%2").arg(
				QString::number(cx),
				QString::number(cy));
		ui->canvasRes->addItem(str, encRes);
	};

	addRes(1920, 1080);
	addRes(1280, 720);

	ui->canvasRes->setCurrentIndex(0);
}

AutoConfigVideoPage::~AutoConfigVideoPage()
{
	delete ui;
}

int AutoConfigVideoPage::nextId() const
{
	return AutoConfig::TestPage;
}

bool AutoConfigVideoPage::validatePage()
{
	int encRes = ui->canvasRes->currentData().toInt();
	wiz->baseResolutionCX = encRes >> 16;
	wiz->baseResolutionCY = encRes & 0xFFFF;
	wiz->fpsType = (AutoConfig::FPSType)ui->fps->currentData().toInt();

	obs_video_info ovi;
	obs_get_video_info(&ovi);

	switch (wiz->fpsType) {
	case AutoConfig::FPSType::PreferHighFPS:
		wiz->specificFPSNum = 0;
		wiz->specificFPSDen = 0;
		wiz->preferHighFPS = true;
		break;
	case AutoConfig::FPSType::PreferHighRes:
		wiz->specificFPSNum = 0;
		wiz->specificFPSDen = 0;
		wiz->preferHighFPS = false;
		break;
	case AutoConfig::FPSType::UseCurrent:
		wiz->specificFPSNum = ovi.fps_num;
		wiz->specificFPSDen = ovi.fps_den;
		wiz->preferHighFPS = false;
		break;
	case AutoConfig::FPSType::fps30:
		wiz->specificFPSNum = 30;
		wiz->specificFPSDen = 1;
		wiz->preferHighFPS = false;
		break;
	case AutoConfig::FPSType::fps60:
		wiz->specificFPSNum = 60;
		wiz->specificFPSDen = 1;
		wiz->preferHighFPS = false;
		break;
	case AutoConfig::FPSType::ServiceSpecified:
		break;
	}

	return true;
}

void AutoConfigStreamPage::UpdateBandwidthTest()
{
	QString qService = obs_data_get_string(serviceSettings, "service");

	std::vector<std::string> disabledBandwidthServices = {
		"youtube"
	};

	bool disabledBandwidthTesting =
		obs_data_get_bool(serviceSettings, "disable_bandwidth_test");

	if (!disabledBandwidthTesting) {
		std::string tService = qService.toStdString();
		std::transform(tService.begin(), tService.end(),
				tService.begin(), tolower);
		for (size_t i = 0; i < disabledBandwidthServices.size(); i++) {
			std::string dService = disabledBandwidthServices[i];
			std::transform(dService.begin(), dService.end(),
					dService.begin(), tolower);
			if (tService.find(dService) != std::string::npos) {
				disabledBandwidthTesting = true;
				break;
			}
		}
	}

	ui->doBandwidthTest->blockSignals(true);
	if (disabledBandwidthTesting) {
		ui->doBandwidthTest->setChecked(false);
		ui->doBandwidthTest->setEnabled(false);
	} else {
		ui->doBandwidthTest->setEnabled(true);
	}
	ui->doBandwidthTest->blockSignals(false);
}

void AutoConfigStreamPage::UpdateBitrate()
{
	bool disabledBitrate =
		obs_data_get_bool(serviceSettings, "disable_bitrate_option");

	ui->bitrateLabel->setHidden(disabledBitrate);
	ui->bitrate->blockSignals(true);
	ui->bitrate->setHidden(disabledBitrate);
	ui->bitrate->setEnabled(disabledBitrate);
	ui->bitrate->blockSignals(false);
}

void AutoConfigStreamPage::StreamSettingsChanged(bool refreshPropertiesView)
{
	/* Store current service temporarily */
	QString qServiceType = "";
	if (ui->streamType->currentIndex() >= 0)
		qServiceType = (ui->streamType->currentData()).toString();

	QString qService = obs_data_get_string(serviceSettings, "service");

	bool custom = qServiceType.toStdString().find("_custom") != std::string::npos;

	blog(LOG_INFO, "service: %s", qServiceType.toStdString().c_str());

	/* Reconstruct properties view */
	if (refreshPropertiesView) {
		OBSData defaults = obs_service_defaults(QT_TO_UTF8(qServiceType));
		obs_data_clear(serviceSettings);
		obs_data_apply(serviceSettings, defaults);
		obs_data_release(defaults);

		streamPropertiesLayout->removeWidget(streamProperties);
		streamProperties->deleteLater();
		streamProperties = new OBSPropertiesView(serviceSettings, qServiceType.toStdString().c_str(),
			(PropertiesReloadCallback)obs_get_service_properties, 0);
		streamProperties->setProperty("changed", QVariant(false));

		QObject::connect(streamProperties, SIGNAL(Changed()),
			this, SLOT(PropertiesChanged()));
		streamPropertiesLayout->addWidget(streamProperties);
	}
	const char* currentSettings = obs_data_get_json(serviceSettings);
	blog(LOG_INFO, "%s", currentSettings);

	UpdateBandwidthTest();
	UpdateBitrate();

	bool testBandwidth = ui->doBandwidthTest->isChecked();

	ui->bitrateLabel->setHidden(testBandwidth);
	ui->bitrate->setHidden(testBandwidth);

	std::vector<std::string> regionBasedServices = {
		"Twitch",
		"Smashcast"
	};

	bool regionBased = false;
	for (size_t i = 0; i < regionBasedServices.size(); i++) {
		if (qService.toStdString().find(regionBasedServices[i]) != std::string::npos) {
			regionBased = true;
			break;
		}
	}

	if (wiz->twitchAuto && qService.toStdString().find("Twitch") != std::string::npos)
		regionBased = false;

	if (custom) {
		ui->region->setVisible(false);
	} else {
		ui->region->setVisible(regionBased && testBandwidth);
	}

	wiz->testRegions = regionBased && testBandwidth;

	obs_data_clear(wiz->serviceSettings);
	obs_data_apply(wiz->serviceSettings, serviceSettings);

	UpdateCompleted();
}

void AutoConfigStreamPage::SettingsChanged()
{
	StreamSettingsChanged(true);
}

void AutoConfigStreamPage::PropertiesChanged()
{
	StreamSettingsChanged(false);
}

/* ------------------------------------------------------------------------- */

AutoConfigStreamPage::AutoConfigStreamPage(QWidget *parent)
	: QWizardPage (parent),
	  ui          (new Ui_AutoConfigStreamPage)
{
	ui->setupUi(this);

	streamPropertiesLayout = new QVBoxLayout(this);

	obs_service_t *service = static_cast<OBSBasic*>(QApplication::activeWindow())->GetService();
	const char *service_type = obs_service_get_type(service);

	serviceSettings = obs_service_get_settings(service);

	streamProperties = new OBSPropertiesView(serviceSettings, service_type,
		(PropertiesReloadCallback)obs_get_service_properties, 0);
	streamProperties->setProperty("changed", QVariant(false));

	QObject::connect(streamProperties, SIGNAL(Changed()),
			this, SLOT(PropertiesChanged()));

	streamPropertiesLayout->addWidget(streamProperties);
	ui->formLayout->insertRow(1, streamPropertiesLayout);

	streamPropertiesLayout->setSizeConstraint(QLayout::SetNoConstraint);
	streamProperties->setSizePolicy(QSizePolicy::Policy::Minimum, QSizePolicy::Policy::MinimumExpanding);

	ui->bitrateLabel->setVisible(false);
	ui->bitrate->setVisible(false);
	ui->region->setVisible(false);

	size_t idx = 0;
	const char *type;

	while (obs_enum_service_types(idx, &type)) {
		const char *name = obs_service_get_display_name(type);
		QString qName = QT_UTF8(name);
		QString qType = QT_UTF8(type);
		ui->streamType->addItem(qName, qType);
		if (strcmp(type, service_type) == 0)
			ui->streamType->setCurrentIndex(idx);
		idx++;
	}

	setTitle(QTStr("Basic.AutoConfig.StreamPage"));
	setSubTitle(QTStr("Basic.AutoConfig.StreamPage.SubTitle"));

	connect(ui->streamType, SIGNAL(currentIndexChanged(int)),
		this, SLOT(SettingsChanged()));

	connect(ui->doBandwidthTest, SIGNAL(toggled(bool)),
		this, SLOT(SettingsChanged()));
	connect(ui->regionUS, SIGNAL(toggled(bool)),
		this, SLOT(SettingsChanged()));
	connect(ui->regionEU, SIGNAL(toggled(bool)),
		this, SLOT(SettingsChanged()));
	connect(ui->regionAsia, SIGNAL(toggled(bool)),
		this, SLOT(SettingsChanged()));
	connect(ui->regionOther, SIGNAL(toggled(bool)),
		this, SLOT(SettingsChanged()));
}

AutoConfigStreamPage::~AutoConfigStreamPage()
{
	delete ui;
	obs_data_release(serviceSettings);
}

bool AutoConfigStreamPage::isComplete() const
{
	return ready;
}

int AutoConfigStreamPage::nextId() const
{
	wiz->serviceSpecifiedFPS = serviceSpecifiedFPS(serviceSettings,
			&wiz->specificFPSNum, &wiz->specificFPSDen,
			&wiz->preferHighFPS);
	return AutoConfig::VideoPage;
}

bool AutoConfigStreamPage::validatePage()
{
	OBSData service_settings = obs_data_create();
	obs_data_release(service_settings);

	int test = ui->streamType->currentIndex();

	std::string qServiceType = "";
	if (ui->streamType->currentIndex() >= 0)
		qServiceType = (ui->streamType->currentData()).toString().toStdString();

	std::string qServiceTypeName = ui->streamType->currentText().toStdString();

	blog(LOG_INFO, "type: %s", qServiceType.c_str());
	blog(LOG_INFO, "name: %s", qServiceTypeName.c_str());

	wiz->customServer = qServiceType.find("_custom") != std::string::npos;

	const char *serverType = qServiceType.c_str();

	const char *json_settings = obs_data_get_json(serviceSettings);
	blog(LOG_INFO, "test_settings: %s", json_settings);

	if (!wiz->customServer) {
		obs_data_set_string(service_settings, "service",
				obs_data_get_string(serviceSettings, "service"));
	}

	OBSService service = obs_service_create(serverType, "temp_service",
			service_settings, nullptr);
	obs_service_release(service);

	int bitrate = 10000;
	bool bandwidthtest = ui->doBandwidthTest->isChecked();
	if (!bandwidthtest) {
		bitrate = ui->bitrate->value();
		wiz->idealBitrate = bitrate;
	}

	OBSData settings = obs_data_create();
	obs_data_release(settings);
	obs_data_set_int(settings, "bitrate", bitrate);
	obs_service_apply_encoder_settings(service, settings, nullptr);

	wiz->serviceType = qServiceType;
	std::string serverName = obs_data_get_string(serviceSettings, "service");
	wiz->serverName = serverName;
	std::string server = obs_data_get_string(serviceSettings, "server");
	wiz->server = server;

	blog(LOG_INFO, "name: %s", wiz->serverName.c_str());
	blog(LOG_INFO, "addr: %s", wiz->server.c_str());

	if (wiz->customServer)
		wiz->serverName = wiz->server;

	wiz->bandwidthTest = bandwidthtest;
	wiz->startingBitrate = (int)obs_data_get_int(settings, "bitrate");
	wiz->idealBitrate = wiz->startingBitrate;
	wiz->regionUS = ui->regionUS->isChecked();
	wiz->regionEU = ui->regionEU->isChecked();
	wiz->regionAsia = ui->regionAsia->isChecked();
	wiz->regionOther = ui->regionOther->isChecked();
	wiz->serviceName = qServiceTypeName;

	if (ui->preferHardware)
		wiz->preferHardware = ui->preferHardware->isChecked();
	wiz->key = obs_data_get_string(serviceSettings, "key");

	if (!wiz->customServer) {
		if (wiz->serviceName == "Twitch")
			wiz->service = AutoConfig::Service::Twitch;
		else if (wiz->serviceName == "Smashcast")
			wiz->service = AutoConfig::Service::Smashcast;
		else
			wiz->service = AutoConfig::Service::Other;
	} else {
		wiz->service = AutoConfig::Service::Other;
	}

	if (wiz->service != AutoConfig::Service::Twitch && wiz->bandwidthTest) {
		QMessageBox::StandardButton button;
#define WARNING_TEXT(x) QTStr("Basic.AutoConfig.StreamPage.StreamWarning." x)
		button = OBSMessageBox::question(this,
				WARNING_TEXT("Title"),
				WARNING_TEXT("Text"));
#undef WARNING_TEXT

		if (button == QMessageBox::No)
			return false;
	}

	return true;
}

static bool validateRequirements(obs_data_t *settings)
{
	obs_data_t *requirementsObj = nullptr;
	obs_data_type type = OBS_DATA_NULL;
	obs_data_item_t *item;
	obs_data_item_t *test_item;
	const char *json_settings = obs_data_get_json(settings);
	blog(LOG_INFO, "%s", json_settings);

	for (item = obs_data_first(settings); item; obs_data_item_next(&item)) {
		obs_data_type item_type = obs_data_item_gettype(item);
		const char *name = obs_data_item_get_name(item);

		if (!obs_data_item_has_user_value(item))
			continue;

		if (strcmp(name, "requirements") == 0) {
			switch (item_type) {
			case OBS_DATA_STRING:
			case OBS_DATA_OBJECT:
				break;
			default:
				return false;
			}
			type = item_type;
			break;
		}
	}

	if (type == OBS_DATA_STRING) {
		const char *name = obs_data_get_string(settings, "requirements");
		test_item = obs_data_item_byname(settings, name);

		if (!obs_data_item_has_user_value(test_item)) {
			blog(LOG_INFO, "%s not found", name);
			obs_data_item_release(&test_item);
			return false;
		}
		blog(LOG_INFO, "%s found", name);
		obs_data_item_release(&test_item);
		return true;
	} else if (type == OBS_DATA_OBJECT) {

	} else {
		return false;
	}

	requirementsObj = obs_data_get_obj(settings, "requirements");
	const char *json = obs_data_get_json(requirementsObj);
	blog(LOG_INFO, "%s", json);
	for (item = obs_data_first(requirementsObj); item; obs_data_item_next(&item)) {
		enum obs_data_type type = obs_data_item_gettype(item);
		const char *name = obs_data_item_get_name(item);
		blog(LOG_INFO, "%s required", name);
		if (!obs_data_item_has_user_value(item))
			return false;

		test_item = obs_data_item_byname(settings, name);

		if (!obs_data_item_has_user_value(test_item)) {
			blog(LOG_INFO, "%s not found", name);
			obs_data_item_release(&test_item);
			return false;
		}
		obs_data_item_release(&test_item);
		blog(LOG_INFO, "%s found", name);
	}

	return true;
}

void AutoConfigStreamPage::UpdateCompleted()
{
	QString key = obs_data_get_string(serviceSettings, "key");

	if (key.isEmpty()) {
		ready = validateRequirements(serviceSettings);
	} else {
		QString qServiceType = ui->streamType->currentData().toString();
		bool custom = qServiceType.toStdString().find("_custom") != std::string::npos;
		if (custom) {
			QString server = obs_data_get_string(serviceSettings, "server");
			ready = !server.isEmpty();
		} else {
			ready = !wiz->testRegions ||
				ui->regionUS->isChecked() ||
				ui->regionEU->isChecked() ||
				ui->regionAsia->isChecked() ||
				ui->regionOther->isChecked();
		}
	}
	emit completeChanged();
}

/* ------------------------------------------------------------------------- */

AutoConfig::AutoConfig(QWidget *parent)
	: QWizard(parent)
{
	calldata_t cd = {0};
	calldata_set_int(&cd, "seconds", 5);

	proc_handler_t *ph = obs_get_proc_handler();
	proc_handler_call(ph, "twitch_ingests_refresh", &cd);
	calldata_free(&cd);

	OBSBasic *main = reinterpret_cast<OBSBasic*>(parent);
	main->EnableOutputs(false);

	installEventFilter(CreateShortcutFilter());

	GetServiceInfo(serviceType, serviceName, server, key);
#ifdef _WIN32
	setWizardStyle(QWizard::ModernStyle);
#endif
	AutoConfigStreamPage *streamPage = new AutoConfigStreamPage();
	AutoConfigVideoPage *videoPage = new AutoConfigVideoPage();
	setPage(StartPage, new AutoConfigStartPage());
	setPage(VideoPage, videoPage);
	setPage(StreamPage, streamPage);
	setPage(TestPage, new AutoConfigTestPage());
	setWindowTitle(QTStr("Basic.AutoConfig"));

	obs_video_info ovi;
	obs_get_video_info(&ovi);

	baseResolutionCX = ovi.base_width;
	baseResolutionCY = ovi.base_height;

	serviceSettings = obs_data_create();

	/* ----------------------------------------- */
	/* check to see if Twitch's "auto" available */

	OBSData twitchSettings = obs_data_create();
	obs_data_release(twitchSettings);

	obs_data_set_string(twitchSettings, "service", "Twitch");

	obs_properties_t *props = obs_get_service_properties("rtmp_common");
	obs_properties_apply_settings(props, twitchSettings);

	obs_property_t *p = obs_properties_get(props, "server");
	const char *first = obs_property_list_item_string(p, 0);
	twitchAuto = strcmp(first, "auto") == 0;

	obs_properties_destroy(props);

	/* ----------------------------------------- */
	/* load service/servers                      */
	customServer = serviceType.find("_custom") != std::string::npos;

	int bitrate = config_get_int(main->Config(), "SimpleOutput", "VBitrate");
	streamPage->ui->bitrate->setValue(bitrate);
	//streamPage->UpdateBandwidthTest();
	streamPage->StreamSettingsChanged(false);

	streamPage->ui->preferHardware->setChecked(os_get_physical_cores() <= 4);

	TestHardwareEncoding();
	if (!hardwareEncodingAvailable) {
		delete streamPage->ui->preferHardware;
		streamPage->ui->preferHardware = nullptr;
	}

	setOptions(0);
	setButtonText(QWizard::FinishButton,
			QTStr("Basic.AutoConfig.ApplySettings"));
	setButtonText(QWizard::BackButton, QTStr("Back"));
	setButtonText(QWizard::NextButton, QTStr("Next"));
	setButtonText(QWizard::CancelButton, QTStr("Cancel"));
}

AutoConfig::~AutoConfig()
{
	OBSBasic *main = reinterpret_cast<OBSBasic*>(App()->GetMainWindow());
	main->EnableOutputs(true);
	obs_data_release(serviceSettings);
}

void AutoConfig::TestHardwareEncoding()
{
	size_t idx = 0;
	const char *id;
	while (obs_enum_encoder_types(idx++, &id)) {
		if (strcmp(id, "ffmpeg_nvenc") == 0)
			hardwareEncodingAvailable = nvencAvailable = true;
		else if (strcmp(id, "obs_qsv11") == 0)
			hardwareEncodingAvailable = qsvAvailable = true;
		else if (strcmp(id, "amd_amf_h264") == 0)
			hardwareEncodingAvailable = vceAvailable = true;
	}
}

bool AutoConfig::CanTestServer(const char *server)
{
	if (!testRegions || (regionUS && regionEU && regionAsia && regionOther))
		return true;

	if (service == Service::Twitch) {
		if (astrcmp_n(server, "US West:", 8) == 0 ||
		    astrcmp_n(server, "US East:", 8) == 0 ||
		    astrcmp_n(server, "US Central:", 11) == 0) {
			return regionUS;
		} else if (astrcmp_n(server, "EU:", 3) == 0) {
			return regionEU;
		} else if (astrcmp_n(server, "Asia:", 5) == 0) {
			return regionAsia;
		} else if (regionOther) {
			return true;
		}
	} else if (service == Service::Smashcast) {
		if (strcmp(server, "Default") == 0) {
			return true;
		} else if (astrcmp_n(server, "US-West:", 8) == 0 ||
		           astrcmp_n(server, "US-East:", 8) == 0) {
			return regionUS;
		} else if (astrcmp_n(server, "EU-", 3) == 0) {
			return regionEU;
		} else if (astrcmp_n(server, "South Korea:", 12) == 0 ||
		           astrcmp_n(server, "Asia:", 5) == 0 ||
		           astrcmp_n(server, "China:", 6) == 0) {
			return regionAsia;
		} else if (regionOther) {
			return true;
		}
	} else {
		return true;
	}

	return false;
}

void AutoConfig::done(int result)
{
	QWizard::done(result);

	if (result == QDialog::Accepted) {
		if (type == Type::Streaming)
			SaveStreamSettings();
		SaveSettings();
	}
}

inline const char *AutoConfig::GetEncoderId(Encoder enc)
{
	switch (enc) {
	case Encoder::NVENC:
		return SIMPLE_ENCODER_NVENC;
	case Encoder::QSV:
		return SIMPLE_ENCODER_QSV;
	case Encoder::AMD:
		return SIMPLE_ENCODER_AMD;
	default:
		return SIMPLE_ENCODER_X264;
	}
};

void AutoConfig::SaveStreamSettings()
{
	OBSBasic *main = reinterpret_cast<OBSBasic*>(App()->GetMainWindow());

	/* ---------------------------------- */
	/* save service                       */

	obs_service_t *oldService = main->GetService();
	OBSData hotkeyData = obs_hotkeys_save_service(oldService);
	obs_data_release(hotkeyData);

	OBSData settings = obs_data_create();
	obs_data_release(settings);
	obs_data_apply(settings, serviceSettings);

	OBSService newService = obs_service_create(serviceType.c_str(),
			"default_service", settings, hotkeyData);
	obs_service_release(newService);

	if (!newService)
		return;

	main->SetService(newService);
	main->SaveService();

	/* ---------------------------------- */
	/* save stream settings               */

	config_set_int(main->Config(), "SimpleOutput", "VBitrate",
			idealBitrate);
	config_set_string(main->Config(), "SimpleOutput", "StreamEncoder",
			GetEncoderId(streamingEncoder));
	config_remove_value(main->Config(), "SimpleOutput", "UseAdvanced");
}

void AutoConfig::SaveSettings()
{
	OBSBasic *main = reinterpret_cast<OBSBasic*>(App()->GetMainWindow());

	if (recordingEncoder != Encoder::Stream)
		config_set_string(main->Config(), "SimpleOutput", "RecEncoder",
				GetEncoderId(recordingEncoder));

	const char *quality = recordingQuality == Quality::High
		? "Small"
		: "Stream";

	config_set_string(main->Config(), "Output", "Mode", "Simple");
	config_set_string(main->Config(), "SimpleOutput", "RecQuality", quality);
	config_set_int(main->Config(), "Video", "BaseCX", baseResolutionCX);
	config_set_int(main->Config(), "Video", "BaseCY", baseResolutionCY);
	config_set_int(main->Config(), "Video", "OutputCX", idealResolutionCX);
	config_set_int(main->Config(), "Video", "OutputCY", idealResolutionCY);

	if (fpsType != FPSType::UseCurrent) {
		config_set_uint(main->Config(), "Video", "FPSType", 0);
		config_set_string(main->Config(), "Video", "FPSCommon",
				std::to_string(idealFPSNum).c_str());
	}

	main->ResetVideo();
	main->ResetOutputs();
	config_save_safe(main->Config(), "tmp", nullptr);
}
