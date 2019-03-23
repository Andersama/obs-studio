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
	id   = user_info->caid;
	
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
	OBSBasic *main = OBSBasic::Get();
	name = get_config_str(main, service(), "Name");
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
	/* TODO: Chat */
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
	std::string tmp = "";
	std::string *token = &tmp;

	QDialog dialog(parent);
	QDialog *prompt = &dialog;
	QFormLayout form(&dialog);
	dialog.setWindowTitle("Caffeine Login");
	
	QDialogButtonBox buttonBox(Qt::Horizontal, &dialog);
	QPushButton *login  = new QPushButton(QTStr("Login"));
	QPushButton *logout = new QPushButton(QTStr("Logout"));
	QPushButton *cancel = new QPushButton(QTStr("Cancel"));

	QLineEdit *u = new QLineEdit(&dialog);
	u->setPlaceholderText(QTStr("Username"));
	form.addWidget(u);
	QLineEdit *p = new QLineEdit(&dialog);
	p->setPlaceholderText(QTStr("Password"));
	p->setEchoMode(QLineEdit::Password);
	form.addWidget(p);

	buttonBox.addButton(login,  QDialogButtonBox::ButtonRole::ActionRole);
	buttonBox.addButton(cancel, QDialogButtonBox::ButtonRole::RejectRole);
	
	auto tryLogin = [=](bool checked) {
		std::string username = u->text().toStdString();
		std::string password = p->text().toStdString();
		std::string otp = "";

		QDialog otpdialog(parent);
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

		int trycount = 0;
retrylogin:
		trycount++;
		struct caffeine_auth_response *response =
			caffeine_signin(username.c_str(), password.c_str(), otp.c_str());
		if (!response) {
			return;
		} else if (response->next) {
			if (strcmp(response->next, "mfa_otp_required") == 0) {
				caffeine_free_auth_response(&response);
				if (otpdialog.exec() == QDialog::Rejected)
					return;
				otp = onetimepassword->text().toStdString();
				if (trycount < 3)
					goto retrylogin;
				return;
			}
			std::string message = "";
			std::string error = "";
			if (strcmp(response->next, "legal_acceptance_required") == 0) {
				message = "Unauthorized";
				error = "Legal acceptance required\n";
			}
			if (strcmp(response->next, "email_verification") == 0) {
				message = "Unauthorized";
				error += "Email needs verification\n";
			}
			caffeine_free_auth_response(&response);

			QString title = QTStr("Auth.ChannelFailure.Title");
			QString text = QTStr("Auth.ChannelFailure.Text")
				.arg("Caffeine", message.c_str(), error.c_str());

			QMessageBox::warning(OBSBasic::Get(), title, text);

			blog(LOG_WARNING, "%s: %s: %s",
				__FUNCTION__,
				message.c_str(),
				error.c_str());

			if (trycount < 3)
				goto retrylogin;
			return;
		} else if (!response->credentials) {
			caffeine_free_auth_response(&response);
			if (trycount < 3)
				goto retrylogin;
			return;
		} else {
			*token = caffeine_refresh_token(response->credentials);
			caffeine_free_auth_response(&response);
			prompt->accept();
		}
	};

	QObject::connect(login, &QPushButton::clicked, tryLogin);
	QObject::connect(&buttonBox, SIGNAL(accepted()), &dialog, SLOT(accept()));
	QObject::connect(&buttonBox, SIGNAL(rejected()), &dialog, SLOT(reject()));
	form.addRow(&buttonBox);
	
	if (dialog.exec() == QDialog::Rejected)
		return nullptr;

	std::shared_ptr<CaffeineAuth> auth = std::make_shared<CaffeineAuth>(caffeineDef);
	if (auth) {
		auth->SetToken(token->c_str());
		if (auth->GetChannelInfo(false))
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
