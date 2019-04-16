#include "auth-caffeine.hpp"

#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>

#include <qt-wrappers.hpp>
#include <obs-app.hpp>

#include "window-basic-main.hpp"
#include "remote-text.hpp"
#include "window-dock.hpp"

#include <util/threading.h>
#include <util/platform.h>

#include <json11.hpp>

#include <ctime>

#include "ui-config.h"
#include "obf.h"

using namespace json11;

#include <browser-panel.hpp>
extern QCef *cef;
extern QCefCookieManager *panel_cookies;
/* ------------------------------------------------------------------------- */

#define CAFFEINE_AUTH_URL \
	"https://obsproject.com/app-auth/caffeine?action=redirect"
#define CAFFEINE_TOKEN_URL \
	"https://obsproject.com/app-auth/caffeine-token"

#define CAFFEINE_SCOPE_VERSION 1

static Auth::Def caffeineDef = {
	"Caffeine",
	Auth::Type::OAuth_StreamKey
};

/* ------------------------------------------------------------------------- */

CaffeineAuth::CaffeineAuth(const Def &d)
	: OAuthStreamKey(d)
{
	UNUSED_PARAMETER(d);
	instance = caff_createInstance();
}

CaffeineAuth::~CaffeineAuth()
{
	caff_freeInstance(&instance);
}

bool CaffeineAuth::GetChannelInfo()
try {
	if (refresh_token.empty()) {
		throw ErrorInfo("Auth Failure", "Could not get refresh token");
	}
	key_ = refresh_token;

	if (!caff_isSignedIn(instance)) {
		auto result = caff_refreshAuth(instance, refresh_token.c_str());
		if (result != caff_ResultSuccess) {
			throw ErrorInfo("Auth Failure", "Signin failed");
		}
	}
	
	username = caff_getUsername(instance);
	
	return true;
} catch (ErrorInfo info) {
	QString title = QTStr("Auth.ChannelFailure.Title");
	QString text = QTStr("Auth.ChannelFailure.Text")
		.arg(service(), info.message.c_str(), info.error.c_str());

	QMessageBox::warning(OBSBasic::Get(), title, text);

	blog(LOG_WARNING, "%s: %s: %s",
			__FUNCTION__,
			info.message.c_str(),
			info.error.c_str());
	return false;
}

void CaffeineAuth::SaveInternal()
{
	OBSBasic *main = OBSBasic::Get();
	config_set_string(main->Config(), service(), "Username", username.c_str());
	if (uiLoaded) {
		config_set_string(main->Config(), service(), "DockState",
				main->saveState().toBase64().constData());
	}
	OAuthStreamKey::SaveInternal();
}

static inline std::string get_config_str(
		OBSBasic *main,
		const char *section,
		const char *name)
{
	const char *val = config_get_string(main->Config(), section, name);
	return val ? val : "";
}

bool CaffeineAuth::LoadInternal()
{
	OBSBasic *main = OBSBasic::Get();
	username = get_config_str(main, service(), "Username");
	firstLoad = false;
	return OAuthStreamKey::LoadInternal();
}

class CaffeineChat : public OBSDock {
public:
	inline CaffeineChat() : OBSDock() {}

	QScopedPointer<QCefWidget> widget;
};

void CaffeineAuth::LoadUI()
{
	if (uiLoaded)
		return;
	if (!GetChannelInfo())
		return;
	/* TODO: Chat */
	uiLoaded = true;
	return;
}

bool CaffeineAuth::RetryLogin()
{
	std::shared_ptr<Auth> ptr = Login(OBSBasic::Get());
	return ptr != nullptr;
}

void CaffeineAuth::TryAuth(
	QLineEdit * u,
	QLineEdit * p,
	QWidget * parent,
	QString const & caffeineStyle,
	QDialog * prompt)
{
	std::string username = u->text().toStdString();
	std::string password = p->text().toStdString();
	std::string otp = "";

	QDialog otpdialog(parent);
	QString style = otpdialog.styleSheet();
	style += caffeineStyle;
	QFormLayout otpform(&otpdialog);
	otpdialog.setWindowTitle("Caffeine Login (One Time Password)");
	//otpform.addRow(new QLabel("Caffeine One Time Password"));

	QLineEdit *onetimepassword = new QLineEdit(&otpdialog);
	onetimepassword->setEchoMode(QLineEdit::Password);
	onetimepassword->setPlaceholderText(QTStr("Password"));
	//otpform.addRow(new QLabel(QTStr("Password")), onetimepassword);
	otpform.addWidget(onetimepassword);

	QPushButton *login = new QPushButton(QTStr("Login"));
	QPushButton *logout = new QPushButton(QTStr("Logout"));
	QPushButton *cancel = new QPushButton(QTStr("Cancel"));

	QDialogButtonBox otpButtonBox(Qt::Horizontal, &otpdialog);

	otpButtonBox.addButton(login, QDialogButtonBox::ButtonRole::AcceptRole);
	otpButtonBox.addButton(cancel, QDialogButtonBox::ButtonRole::RejectRole);

	QObject::connect(&otpButtonBox, SIGNAL(accepted()), &otpdialog, SLOT(accept()));
	QObject::connect(&otpButtonBox, SIGNAL(rejected()), &otpdialog, SLOT(reject()));
	otpform.addRow(&otpButtonBox);

	std::string message = "";
	std::string error = "";

	if (username.empty() || password.empty()) {
		message = "Missing Password or Username";
		error = "A username and password are required!";
		QString title = QTStr("Auth.ChannelFailure.Title");
		QString text = QTStr("Auth.ChannelFailure.Text")
			.arg("Caffeine", message.c_str(), error.c_str());

		QMessageBox::warning(OBSBasic::Get(), title, text);
		return;
	}

	auto response = caff_signIn(instance, username.c_str(), password.c_str(), otp.c_str());
	switch (response) {
	case caff_ResultSuccess:
		refresh_token = caff_getRefreshToken(instance);
		prompt->accept();
		return;
	case caff_ResultInfoIncorrect:
		message = "Unauthorized";
		error = "Incorrect login info";
		break;
	case caff_ResultOldVersion:
		message = "Unauthorized";
		error = "Out-of-date version of libcaffeine";
		break;
	case caff_ResultMfaOtpRequired:
	case caff_ResultMfaOtpIncorrect: /* TODO make this different */
		if (otpdialog.exec() == QDialog::Rejected)
			return;
		otp = onetimepassword->text().toStdString();
		return;
	case caff_ResultLegalAcceptanceRequired:
		message = "Unauthorized";
		error = "Legal acceptance required\n";
		break;
	case caff_ResultEmailVerificationRequired:
		message = "Unauthorized";
		error = "Email needs verification\n";
		break;
	case caff_ResultFailure:
	default:
		message = "Failed";
		error = "Sign-in request failed";
		break;
	}

	QString title = QTStr("Auth.ChannelFailure.Title");
	QString text = QTStr("Auth.ChannelFailure.Text")
		.arg("Caffeine", message.c_str(), error.c_str());

	QMessageBox::warning(OBSBasic::Get(), title, text);

	blog(LOG_WARNING, "%s: %s: %s",
		__FUNCTION__,
		message.c_str(),
		error.c_str());
	return;
}

std::shared_ptr<Auth> CaffeineAuth::Login(QWidget *parent)
{
	QDialog dialog(parent);
	QDialog *prompt = &dialog;
	QFormLayout form(&dialog);
	form.setContentsMargins(151, 101, 151, 101);
	form.setSpacing(10);
	dialog.setObjectName("caffeinelogin");
	dialog.setProperty("themeID", "caffeineLogin");
	QString caffeineStyle = "\
		* [themeID=\"caffeineLogo\"] {padding-left: 50px; padding-right: 50px; padding-bottom: 20px; background-color:white;}\
		* [themeID=\"caffeineWelcome\"] {font-weight: normal; font-family: SegoeUI, sans-serif; letter-spacing: 0.1px; line-height: 53px; font-size: 40px; background-color:white; color:#000;}\
		* [themeID=\"caffeineIntro\"] {padding-bottom: 10px; font-weight: normal; font-family: SegoeUI, sans-serif; letter-spacing: 0px; line-height: 43px; font-size: 32px; background-color:white; color:#222;}\
		QLineEdit {padding-left: 29px; padding-right: 29px; padding-bottom: 20px; padding-top: 20px; font-weight: normal; font-family: SegoeUI, sans-serif; border-radius: 5px; border: 1px solid #8b8b8b;}\
		QPushButton {font-weight: normal; font-family: SegoeUI, sans-serif; font-size: 36px; background-color: #009fe0; color:#FFF; border-radius: 36px; padding-left: 90px; padding-right: 90px; border: 1px solid #009fe0}\
		QPushButton::hover {background-color:#007cad;}\
		* [themeID=\"caffeineLogin\"] {font-weight: normal; font-family: SegoeUI, sans-serif; letter-spacing: 0.1px; line-height: 24px; font-size: 18px; background-color:white; color:#000;}\
		* [themeID=\"caffeineTrouble\"] {padding-left: 29px; padding-right: 29px; font-weight: normal; font-family: SegoeUI, sans-serif; letter-spacing: 0.1px; line-height: 24px; font-size: 18px; background-color:white; color:#000;}";

	QString style = dialog.styleSheet();
	style += caffeineStyle;

	dialog.setStyleSheet(style);
	dialog.setWindowTitle("Caffeine Login");
	
	QDialogButtonBox buttonBox(Qt::Horizontal, &dialog);
	QLabel *logo = new QLabel();
	QPixmap image(":/res/images/CaffeineLogo.png");
	logo->setPixmap(image.scaled(logo->size(), Qt::KeepAspectRatio,
			Qt::SmoothTransformation));
	logo->setAlignment(Qt::AlignHCenter);
	logo->setProperty("themeID", "caffeineLogo");

	form.addRow(logo);
	QLabel *welcome = new QLabel("Welcome to Caffeine");
	welcome->setAlignment(Qt::AlignHCenter);
	welcome->setProperty("themeID", "caffeineWelcome");
	QLabel *intro = new QLabel("Sign in");
	intro->setAlignment(Qt::AlignHCenter);
	intro->setProperty("themeID", "caffeineIntro");
	form.addRow(welcome);
	form.addRow(intro);

	QPushButton *signin  = new QPushButton(QTStr("Sign In"));
	signin->setMinimumHeight(72);
	QPushButton *logout = new QPushButton(QTStr("Sign Out"));
	QLabel      *trouble = new QLabel(
		"<a href=\"https://www.caffeine.tv/forgot-password\">"
		+ QTStr("Trouble Signing In?") + "</a>"
	);
	trouble->setProperty("themeID", "caffeineTrouble");
	QLabel      *signup = new QLabel(
		"New to Caffeine? <a href=\"https://www.caffeine.tv/sign-up\">"
		+ QTStr("Sign Up") + "</a>"
	);
	buttonBox.setCenterButtons(true);
	buttonBox.addButton(signin,  QDialogButtonBox::ButtonRole::ActionRole);

	signup->setAlignment(Qt::AlignHCenter);
	signup->setProperty("themeID", "caffeineLogin");

	QLineEdit *u = new QLineEdit(&dialog);
	u->setPlaceholderText(QTStr("Username"));
	u->setProperty("themeID", "caffeineLogin");
	u->setMinimumHeight(56);
	form.addRow(u);

	QLineEdit *p = new QLineEdit(&dialog);
	p->setPlaceholderText(QTStr("Password"));
	p->setEchoMode(QLineEdit::Password);
	p->setProperty("themeID", "caffeineLogin");
	p->setMinimumHeight(56);

	form.addRow(p);
	form.addRow(trouble);
	form.addRow(&buttonBox);
	form.addRow(signup);
	
	QObject::connect(&buttonBox, SIGNAL(accepted()), &dialog, SLOT(accept()));
	QObject::connect(&buttonBox, SIGNAL(rejected()), &dialog, SLOT(reject()));

	std::shared_ptr<CaffeineAuth> auth = std::make_shared<CaffeineAuth>(caffeineDef);
	QObject::connect(signin, &QPushButton::clicked,
		[=](bool checked) { auth->TryAuth(u, p, parent, caffeineStyle, prompt); });
	
	if (dialog.exec() == QDialog::Rejected)
		return nullptr;

	if (auth) {
		if (auth->GetChannelInfo())
			return auth;
	}
	return nullptr;
}

static std::shared_ptr<Auth> CreateCaffeineAuth()
{
	return std::make_shared<CaffeineAuth>(caffeineDef);
}

static void DeleteCookies()
{

}

void RegisterCaffeineAuth()
{
	OAuth::RegisterOAuth(
			caffeineDef,
			CreateCaffeineAuth,
			CaffeineAuth::Login,
			DeleteCookies);	
}
