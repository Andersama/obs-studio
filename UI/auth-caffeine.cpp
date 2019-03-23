#include "auth-caffeine.hpp"

#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
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
}

bool CaffeineAuth::GetChannelInfo(bool allow_retry)
try {
	if (refresh_token.empty()) {
		if (allow_retry && RetryLogin())
			return GetChannelInfo(false);
		throw ErrorInfo("Auth Failure", "Could not get refresh token");
	}
	key_ = refresh_token;
	struct caffeine_credentials *credentials =
		caffeine_refresh_auth(refresh_token.c_str());

	if (!credentials) {
		if (allow_retry && RetryLogin())
			return GetChannelInfo(false);
		throw ErrorInfo("Auth Failure", "Could not get credentials");
	}
	
	struct caffeine_user_info *user_info = caffeine_getuser(credentials);
	if (!user_info) {
		caffeine_free_credentials(&credentials);
		if (allow_retry && RetryLogin())
			return GetChannelInfo(false);
		throw ErrorInfo("Auth Failure", "Could not get user info");
	}
	
	name = user_info->username;
	id = user_info->caid; //"caffeine";
	
	caffeine_free_user_info(&user_info);
	caffeine_free_credentials(&credentials);
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
	config_set_string(main->Config(), service(), "Name", name.c_str());
	config_set_string(main->Config(), service(), "Id", id.c_str());
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
	/*
	if (!cef)
		return false;
		*/
	OBSBasic *main = OBSBasic::Get();
	name = get_config_str(main, service(), "Name");
	username = name;
	id = get_config_str(main, service(), "Id");
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
	uiLoaded = true;
	return;
}

bool CaffeineAuth::RetryLogin()
{
	std::shared_ptr<Auth> ptr = Login(OBSBasic::Get());
	return ptr != nullptr;
}

void CaffeineAuth::SetToken(std::string token)
{
	refresh_token = token;
}

std::shared_ptr<Auth> CaffeineAuth::Login(QWidget *parent)
{	
	QDialog dialog(parent);
	QFormLayout form(&dialog);
	form.addRow(new QLabel("Caffeine Login"));
	
	QDialogButtonBox buttonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
			Qt::Horizontal, &dialog);
			
	QLineEdit *u = new QLineEdit(&dialog);
	form.addRow(new QLabel(QTStr("Username")), u);
	QLineEdit *p = new QLineEdit(&dialog);
	form.addRow(new QLabel(QTStr("Password")), p);
			
	QObject::connect(&buttonBox, SIGNAL(accepted()), &dialog, SLOT(accept()));
	QObject::connect(&buttonBox, SIGNAL(rejected()), &dialog, SLOT(reject()));
	form.addRow(&buttonBox);
	
	if (dialog.exec() == QDialog::Rejected)
		return nullptr;

	std::string username;
	std::string password;
	std::string otp;
	username = u->text().toStdString();
	password = p->text().toStdString();
	
	QDialog otpdialog(parent);
	QFormLayout otpform(&otpdialog);
	otpform.addRow(new QLabel("Caffeine One Time Password"));
	
	QLineEdit *onetimepassword = new QLineEdit(&otpdialog);
	otpform.addRow(new QLabel(QTStr("Password")), onetimepassword);
	
	QDialogButtonBox otpButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
		Qt::Horizontal, &otpdialog);
		
	QObject::connect(&otpButtonBox, SIGNAL(accepted()), &otpdialog, SLOT(accept()));
	QObject::connect(&otpButtonBox, SIGNAL(rejected()), &otpdialog, SLOT(reject()));
	otpform.addRow(&otpButtonBox);
	
	int trycount = 0;
retrylogin:	
	trycount++;
	struct caffeine_auth_response *response =
		caffeine_signin(username.c_str(), password.c_str(), otp.c_str());
	if (!response) {
		return nullptr;
	} else if (response->next) {
		if (strcmp(response->next, "mfa_otp_required") == 0) {
			caffeine_free_auth_response(&response);
			if (otpdialog.exec() == QDialog::Rejected)
				return nullptr;
			otp = onetimepassword->text().toStdString();
			if(trycount > 3)
				goto retrylogin;
			else
				return nullptr;
		}
		if (strcmp(response->next, "legal_acceptance_required") == 0) {
			//show_message(props, obs_module_text("TosAcceptanceRequired"));
		}
		if (strcmp(response->next, "email_verification") == 0) {
			//show_message(props, obs_module_text("EmailVerificationRequired"));
		}
		caffeine_free_auth_response(&response);
		return nullptr;
	} else if (!response->credentials) {
		caffeine_free_auth_response(&response);
		if(trycount > 3)
			goto retrylogin;
		else
			return nullptr;
	} else {
		std::shared_ptr<CaffeineAuth> auth = std::make_shared<CaffeineAuth>(caffeineDef);
		if (auth)
			auth->SetToken(caffeine_refresh_token(response->credentials));

		caffeine_free_auth_response(&response);
		if (auth->GetChannelInfo(false))
			return auth;
	}
	
	return nullptr;
	/*
	OAuthLogin login(parent, CAFFEINE_AUTH_URL, false);
	cef->add_popup_whitelist_url("about:blank", &login);

	if (login.exec() == QDialog::Rejected) {
		return nullptr;
	}

	std::shared_ptr<CaffeineAuth> auth = std::make_shared<CaffeineAuth>(caffeineDef);

	std::string client_id = CAFFEINE_CLIENTID;
	deobfuscate_str(&client_id[0], CAFFEINE_HASH);

	if (!auth->GetToken(CAFFEINE_TOKEN_URL, client_id, CAFFEINE_SCOPE_VERSION,
				QT_TO_UTF8(login.GetCode()))) {
		return nullptr;
	}

	std::string error;
	if (auth->GetChannelInfo(false)) {
		return auth;
	}

	return nullptr;
	*/
}

static std::shared_ptr<Auth> CreateCaffeineAuth()
{
	return std::make_shared<CaffeineAuth>(caffeineDef);
}

static void DeleteCookies()
{
	/*
	if (panel_cookies)
		panel_cookies->DeleteCookies("caffeine.tv", std::string());
		*/
}

void RegisterCaffeineAuth()
{
	OAuth::RegisterOAuth(
			caffeineDef,
			CreateCaffeineAuth,
			CaffeineAuth::Login,
			DeleteCookies);	
}
