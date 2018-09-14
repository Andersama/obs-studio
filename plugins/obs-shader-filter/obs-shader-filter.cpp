#include "obs-shader-filter.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs_shader_filter", "en-US")
#define blog(level, msg, ...) blog(level, "shader-filter: " msg, ##__VA_ARGS__)

static void sidechain_capture(void *p, obs_source_t *source,
		const struct audio_data *audio_data, bool muted);

static bool shader_filter_reload_effect_clicked(obs_properties_t *props,
	obs_property_t *property, void *data);

static bool shader_filter_file_name_changed(obs_properties_t *props,
	obs_property_t *p, obs_data_t *settings);

static const char *shader_filter_texture_file_filter =
"Textures (*.bmp *.tga *.png *.jpeg *.jpg *.gif);;";

static const char *shader_filter_media_file_filter =
"Video Files (*.mp4 *.ts *.mov *.wmv *.flv *.mkv *.avi *.gif *.webm);;";

#define M_PI_D 3.141592653589793238462643383279502884197169399375
static double hlsl_clamp(double in, double min, double max)
{
	if (in < min)
		return min;
	if (in > max)
		return max;
	return in;
}

static double hlsl_degrees(double radians)
{
	return radians * (180.0 / M_PI_D);
}

static double hlsl_rad(double degrees)
{
	return degrees * (M_PI_D / 180.0);
}

static double audio_mel_from_hz(double hz)
{
	return 2595 * log10(1 + hz / 700.0);
}

static double audio_hz_from_mel(double mel)
{
	return 700 * (pow(10, mel / 2595) - 1);
}

const static double flt_max = FLT_MAX;
const static double flt_min = FLT_MIN;
const static double int_min = INT_MIN;
const static double int_max = INT_MAX;
static double sample_rate;
static double output_channels;

/* Additional likely to be used functions for mathmatical expressions */
void prepFunctions(std::vector<te_variable> *vars)
{
	std::vector<te_variable> funcs = {{"clamp", hlsl_clamp, TE_FUNCTION3},
			{"float_max", &flt_max}, {"float_min", &flt_min},
			{"int_max", &int_max}, {"int_min", &int_min},
			{"sample_rate", &sample_rate},
			{"channels", &output_channels},
			{"mel_from_hz", audio_mel_from_hz, TE_FUNCTION1},
			{"hz_from_mel", audio_hz_from_mel, TE_FUNCTION1},
			{"degrees", hlsl_degrees, TE_FUNCTION1},
			{"radians", hlsl_rad, TE_FUNCTION1},
			{"random", random_double, TE_FUNCTION2} };
	vars->reserve(vars->size() + funcs.size());
	vars->insert(vars->end(), funcs.begin(), funcs.end());
}

std::string toSnakeCase(std::string str)
{
	size_t i;
	char c;
	for (i = 0; i < str.size(); i++) {
		c = str[i];
		if (isupper(c)) {
			str.insert(i++, "_");
			str.assign(i, (char)tolower(c));
		}
	}
	return str;
}

std::string toCamelCase(std::string str)
{
	size_t i;
	char c;
	for (i = 0; i < str.size(); i++) {
		c = str[i];
		if (c == '_') {
			str.erase(i);
			if (i < str.size())
				str.assign(i, (char)toupper(c));
		}
	}
	return str;
}

int getDataSize(enum gs_shader_param_type type)
{
	switch (type) {
	case GS_SHADER_PARAM_VEC4:
	case GS_SHADER_PARAM_INT4:
		return 4;
	case GS_SHADER_PARAM_VEC3:
	case GS_SHADER_PARAM_INT3:
		return 3;
	case GS_SHADER_PARAM_VEC2:
	case GS_SHADER_PARAM_INT2:
		return 2;
	case GS_SHADER_PARAM_FLOAT:
	case GS_SHADER_PARAM_INT:
	case GS_SHADER_PARAM_BOOL:
		return 1;
	case GS_SHADER_PARAM_MATRIX4X4:
		return 16;
	}
	return 0;
}

bool isFloatType(enum gs_shader_param_type type)
{
	switch (type) {
	case GS_SHADER_PARAM_VEC4:
	case GS_SHADER_PARAM_VEC3:
	case GS_SHADER_PARAM_VEC2:
	case GS_SHADER_PARAM_FLOAT:
	case GS_SHADER_PARAM_MATRIX4X4:
		return true;
	}
	return false;
}

bool isIntType(enum gs_shader_param_type type)
{
	switch (type) {
	case GS_SHADER_PARAM_INT:
	case GS_SHADER_PARAM_INT2:
	case GS_SHADER_PARAM_INT3:
	case GS_SHADER_PARAM_INT4:
		return true;
	}
	return false;
}

class EVal {
public:
	float defaultFloat = 0.0;
	int defaultInt = 0;

	void *data = nullptr;
	size_t size = 0;
	gs_shader_param_type type = GS_SHADER_PARAM_UNKNOWN;
	EVal()
	{
	};
	~EVal()
	{
		if (data)
			bfree(data);
	};

	operator std::vector<float>()
	{
		std::vector<float> d_float;
		std::vector<int> d_int;
		std::vector<bool> d_bool;
		float *ptr_float = static_cast<float*>(data);
		int *ptr_int = static_cast<int*>(data);
		bool *ptr_bool = static_cast<bool*>(data);

		size_t i;
		size_t len;

		switch (type) {
		case GS_SHADER_PARAM_BOOL:
			len = size / sizeof(bool);
			d_float.reserve(len);
			d_bool.assign(ptr_bool, ptr_bool + len);
			for (i = 0; i < d_bool.size(); i++)
				d_float.push_back(d_bool[i]);
			break;
		case GS_SHADER_PARAM_FLOAT:
		case GS_SHADER_PARAM_VEC2:
		case GS_SHADER_PARAM_VEC3:
		case GS_SHADER_PARAM_VEC4:
		case GS_SHADER_PARAM_MATRIX4X4:
			len = size / sizeof(float);
			d_float.assign(ptr_float, ptr_float + len);
			break;
		case GS_SHADER_PARAM_INT:
		case GS_SHADER_PARAM_INT2:
		case GS_SHADER_PARAM_INT3:
		case GS_SHADER_PARAM_INT4:
			len = size / sizeof(int);
			d_float.reserve(len);
			d_int.assign(ptr_int, ptr_int + len);
			for (i = 0; i < d_int.size(); i++)
				d_float.push_back((float)d_int[i]);
			break;
		}
		return d_float;
	}

	operator std::vector<int>()
	{
		std::vector<float> d_float;
		std::vector<int> d_int;
		std::vector<bool> d_bool;
		float *ptr_float = static_cast<float*>(data);
		int *ptr_int = static_cast<int*>(data);
		bool *ptr_bool = static_cast<bool*>(data);

		size_t i;
		size_t len;

		switch (type) {
		case GS_SHADER_PARAM_BOOL:
			len = size / sizeof(bool);
			d_int.reserve(len);
			d_bool.assign(ptr_bool, ptr_bool + len);
			for (i = 0; i < d_bool.size(); i++)
				d_int.push_back(d_bool[i]);
			break;
		case GS_SHADER_PARAM_FLOAT:
		case GS_SHADER_PARAM_VEC2:
		case GS_SHADER_PARAM_VEC3:
		case GS_SHADER_PARAM_VEC4:
		case GS_SHADER_PARAM_MATRIX4X4:
			len = size / sizeof(float);
			d_int.reserve(len);
			d_float.assign(ptr_float, ptr_float + len);
			for (i = 0; i < d_float.size(); i++)
				d_int.push_back((int)d_float[i]);
			break;
		case GS_SHADER_PARAM_INT:
		case GS_SHADER_PARAM_INT2:
		case GS_SHADER_PARAM_INT3:
		case GS_SHADER_PARAM_INT4:
			len = size / sizeof(int);
			d_int.assign(ptr_int, ptr_int + len);
			break;
		}
		return d_int;
	}

	operator std::vector<bool>()
	{
		std::vector<float> d_float;
		std::vector<int> d_int;
		std::vector<bool> d_bool;
		float *ptr_float = static_cast<float*>(data);
		int *ptr_int = static_cast<int*>(data);
		bool *ptr_bool = static_cast<bool*>(data);

		size_t i;
		size_t len;

		switch (type) {
		case GS_SHADER_PARAM_BOOL:
			len = size / sizeof(bool);
			d_bool.assign(ptr_bool, ptr_bool + len);
			break;
		case GS_SHADER_PARAM_FLOAT:
		case GS_SHADER_PARAM_VEC2:
		case GS_SHADER_PARAM_VEC3:
		case GS_SHADER_PARAM_VEC4:
		case GS_SHADER_PARAM_MATRIX4X4:
			len = size / sizeof(float);
			d_float.assign(ptr_float, ptr_float + len);
			d_bool.reserve(len);
			for (i = 0; i < d_float.size(); i++)
				d_bool.push_back(d_float[i]);
			break;
		case GS_SHADER_PARAM_INT:
		case GS_SHADER_PARAM_INT2:
		case GS_SHADER_PARAM_INT3:
		case GS_SHADER_PARAM_INT4:
			len = size / sizeof(int);
			d_int.assign(ptr_int, ptr_int + len);
			d_bool.reserve(len);
			for (i = 0; i < d_int.size(); i++)
				d_bool.push_back(d_int[i]);
			break;
		}
		return d_bool;
	}
	operator std::string()
	{
		std::string str = "";
		char *ptr_char = static_cast<char*>(data);

		switch (type) {
		case GS_SHADER_PARAM_STRING:
			str = ptr_char;
			break;
		}
		return str;
	}

	std::string getString()
	{
		return *this;
	}

	const char *c_str()
	{
		return ((std::string)*this).c_str();
	}
};

class EParam {
private:
	EVal *getValue(gs_eparam_t *eparam)
	{
		EVal *v = nullptr;

		if (eparam) {
			gs_effect_param_info note_info;
			gs_effect_get_param_info(eparam, &note_info);

			v = new EVal();
			v->data = gs_effect_get_default_val(eparam);
			v->size = gs_effect_get_default_val_size(eparam);
			v->type = note_info.type;
		}

		return v;
	}
protected:
	gs_eparam_t *_param = nullptr;
	gs_effect_param_info _param_info = { 0 };
	EVal *_value = nullptr;
	std::unordered_map<std::string, EParam *> _annotations_map;
	size_t _annotationCount;
public:
	std::unordered_map<std::string, EParam *> *getAnnootations()
	{
		return &_annotations_map;
	}

	gs_effect_param_info info() const
	{
		return _param_info;
	}

	EVal *getValue()
	{
		return _value ? _value : (_value = getValue(_param));
	}

	gs_eparam_t *getParam()
	{
		return _param;
	}

	operator gs_eparam_t*()
	{
		return _param;
	}

	size_t getAnnotationCount()
	{
		return _annotations_map.size();
	}

	/* Hash Map Search */
	EParam *getAnnotation(std::string name)
	{
		if (_annotations_map.find(name) != _annotations_map.end())
			return _annotations_map.at(name);
		else
			return nullptr;
	}

	EParam *operator[](std::string name)
	{
		return getAnnotation(name);
	}

	EVal *getAnnotationValue(std::string name)
	{
		EParam *note = getAnnotation(name);
		if (note)
			return note->getValue();
		else
			return nullptr;
	}


	bool hasAnnotation(std::string name)
	{
		return _annotations_map.find(name) != _annotations_map.end();
	}

	EParam(gs_eparam_t *param)
	{
		_param = param;
		gs_effect_get_param_info(param, &_param_info);
		_value = getValue(param);

		size_t i;
		_annotationCount = gs_param_get_num_annotations(_param);
		_annotations_map.reserve(_annotationCount);

		gs_eparam_t *p = nullptr;
		std::vector<EParam *>::iterator annotation_it;
		std::vector<gs_effect_param_info>::iterator info_it;

		for (i = 0; i < _annotationCount; i++) {
			p = gs_param_get_annotation_by_idx(_param, i);
			EParam *ep = new EParam(p);
			gs_effect_param_info _info;
			gs_effect_get_param_info(p, &_info);

			_annotations_map.insert(std::pair<std::string, EParam *>
					(_info.name, ep));
		}
	}

	~EParam()
	{
		if (_value)
			delete _value;
		for(const auto &annotation : _annotations_map)
			delete annotation.second;
		_annotations_map.clear();
	}

	template <class DataType>
	void setValue(DataType *data, size_t size)
	{
		size_t len = size / sizeof(DataType);
		size_t arraySize = len * sizeof(DataType);
		gs_effect_set_val(_param, data, arraySize);
	}

	template <class DataType>
	void setValue(std::vector<DataType> data)
	{
		size_t arraySize = data.size() * sizeof(DataType);
		gs_effect_set_val(_param, data.data(), arraySize);
	}
};

class ShaderData {
protected:
	gs_shader_param_type _paramType;

	ShaderFilter *_filter;
	ShaderParameter *_parent;
	EParam *_param;

	std::vector<out_shader_data> _values;
	std::vector<in_shader_data> _bindings;

	std::vector<std::string> _names;
	std::vector<std::string> _descs;
	std::vector<std::string> _tooltips;
	std::vector<std::string> _binding_names;
	std::vector<std::string> _expressions;

	size_t _dataCount;
public:
	gs_shader_param_type getParamType() const
	{
		return _paramType;
	}

	ShaderParameter *getParent()
	{
		return _parent;
	}

	ShaderData(ShaderParameter *parent = nullptr,
			ShaderFilter *filter = nullptr) :
			_parent(parent),
			_filter(filter)
	{
		if (_parent)
			_param = _parent->getParameter();
		else
			_param = nullptr;
	}

	virtual ~ShaderData()
	{
	};

	virtual void init(gs_shader_param_type paramType)
	{
		_paramType = paramType;
		_dataCount = getDataSize(paramType);

		_names.reserve(_dataCount);
		_descs.reserve(_dataCount);
		_values.reserve(_dataCount);
		_bindings.reserve(_dataCount);
		_expressions.reserve(_dataCount);
		_binding_names.reserve(_dataCount);
		_tooltips.reserve(_dataCount);

		size_t i;
		out_shader_data empty = { 0 };
		in_shader_data emptyBinding = { 0 };

		std::string n = _parent->getName();
		std::string d = _parent->getDescription();
		std::string strNum = "";
		EVal *val = nullptr;
		for (i = 0; i < _dataCount; i++) {
			if(_dataCount > 1)
				strNum = "_" + std::to_string(i);
			_names.push_back(n + strNum);
			_descs.push_back(d + strNum);
			_binding_names.push_back(toSnakeCase(_names[i]));
			_tooltips.push_back(_binding_names[i]);
			_values.push_back(empty);
			_bindings.push_back(emptyBinding);

			val = _param->getAnnotationValue("expr"+strNum);
			if (val)
				_expressions.push_back(*val);
			else
				_expressions.push_back("");

			te_variable var = { 0 };
			var.address = &_bindings[i];
			var.name = _binding_names[i].c_str();
			if (_filter)
				_filter->appendVariable(var);
		}

		std::string dir[4] = { "left","right","top","bottom" };
		for (i = 0; i < 4; i++) {
			if (_filter->resizeExpressions[i].empty()) {
				val = _param->getAnnotationValue("resize_expr_" + dir[i]);
				if (val)
					_filter->resizeExpressions[i] = val->getString();
			}
		}
	};

	virtual void getProperties(ShaderFilter *filter, obs_properties_t *props)
	{
		UNUSED_PARAMETER(filter);
		UNUSED_PARAMETER(props);
	};

	virtual void videoTick(ShaderFilter *filter, float elapsedTime,
			float seconds)
	{
		UNUSED_PARAMETER(filter);
		UNUSED_PARAMETER(elapsedTime);
		UNUSED_PARAMETER(seconds);
	};

	virtual void videoRender(ShaderFilter *filter)
	{
		UNUSED_PARAMETER(filter);
	};

	virtual void update(ShaderFilter *filter)
	{
		UNUSED_PARAMETER(filter);
	};
};

class NumericalData : public ShaderData {
private:
	void fillIntList(EParam *e, obs_property_t *p)
	{
		std::unordered_map<std::string, EParam *> *notations =
				e->getAnnootations();
		for (std::unordered_map<std::string, EParam *>::iterator it =
				notations->begin(); it != notations->end();
				it++) {
			EParam *eparam = (*it).second;
			EVal *eval = eparam->getValue();
			std::string name = eparam->info().name;

			if (name.compare(0, 9, "list_item") == 0 &&
					name.compare(name.size() - 6, 5, "_name") != 0) {
				std::vector<int> iList = *eval;
				if (iList.size()) {
					EVal *evalname = e->getAnnotationValue((name + "_name"));
					std::string itemname = *evalname;
					int d = iList[0];
					if (itemname.empty())
						itemname = std::to_string(d);
					obs_property_list_add_int(p,
							itemname.c_str(), d);
				}
			}
		}
	}

	void fillFloatList(EParam *e, obs_property_t *p)
	{
		std::unordered_map<std::string, EParam *> *notations =
			e->getAnnootations();
		for (std::unordered_map<std::string, EParam *>::iterator it =
			notations->begin(); it != notations->end();
			it++) {
			EParam *eparam = (*it).second;
			EVal *eval = eparam->getValue();
			std::string name = eparam->info().name;
			gs_shader_param_type type = eparam->info().type;

			if (name.compare(0, 9, "list_item") == 0 &&
				name.compare(name.size() - 6, 5, "_name") != 0) {
				std::vector<float> fList = *eval;
				if (fList.size()) {
					EVal *evalname = e->getAnnotationValue((name + "_name"));
					std::string itemname = *evalname;
					double d = fList[0];
					if (itemname.empty())
						itemname = std::to_string(d);
					obs_property_list_add_float(p,
							itemname.c_str(), d);
				}
			}
		}
	}

	void fillComboBox(EParam *e, obs_property_t *p)
	{
		EVal *enabledval = e->getAnnotationValue("enabled_desc");
		EVal *disabledval = e->getAnnotationValue("disabled_desc");
		std::string enabled = _OMT("On");
		std::string disabled = _OMT("Off");
		if (enabledval) {
			std::string temp = *enabledval;
			if (!temp.empty())
				enabled = temp;
		}
		if (disabledval) {
			std::string temp = *disabledval;
			if (!temp.empty())
				disabled = temp;
		}
		obs_property_list_add_int(p, enabled.c_str(), 1);
		obs_property_list_add_int(p, disabled.c_str(), 0);
	}
protected:
	bool _isFloat;
	bool _isInt;
	bool _isSlider;
	bool _skipWholeProperty;
	bool _skipCalculations;
	bool _showExpressionLess;
	std::vector<bool> _skipProperty;
	std::vector<bool> _disableProperty;
	double _min;
	double _max;
	double _step;
	enum BindType {
		unspecified,
		none,
		byte,
		short_integer,
		integer,
		floating_point,
		double_point
	};
	void *_bind = nullptr;
	BindType bindType;
	enum NumericalType {
		combobox,
		list,
		num,
		slider,
		color
	};
	NumericalType _numType;
public:
	NumericalData(ShaderParameter *parent, ShaderFilter *filter) :
		ShaderData(parent, filter)
	{
		gs_eparam_t *param = parent->getParameter()->getParam();
		struct gs_effect_param_info info;
		gs_effect_get_param_info(param, &info);
		/* std::vector<DataType> *bind */
		std::string n = info.name;
		if (n == "ViewProj") {
			bindType = floating_point;
			_bind = &_filter->view_proj;
		} else if (n == "uv_offset") {
			bindType = floating_point;
			_bind = &_filter->uvOffset;
		} else if (n == "uv_scale") {
			bindType = floating_point;
			_bind = &_filter->uvScale;
		} else if (n == "uv_pixel_interval") {
			bindType = floating_point;
			_bind = &_filter->uvPixelInterval;
		} else if (n == "elapsed_time") {
			bindType = floating_point;
			_bind = &_filter->elapsedTime;
		}
	};

	~NumericalData()
	{
	};

	void init(gs_shader_param_type paramType)
	{
		ShaderData::init(paramType);
		_isFloat = isFloatType(paramType);
		_isInt = isIntType(paramType);
		_skipWholeProperty = _bind ? true : false;
		_skipCalculations = false;
		size_t i;
		if (_isFloat) {
			_min = -FLT_MAX;
			_max = FLT_MAX;
			_step = 1.0;
		} else {
			_min = INT_MIN;
			_max = INT_MAX;
			_step = 1;
		}
		EVal *min = _param->getAnnotationValue("min");
		EVal *max = _param->getAnnotationValue("max");
		EVal *step = _param->getAnnotationValue("step");
		if (min)
			_min = ((std::vector<float>)*min)[0];
		if (max)
			_max = ((std::vector<float>)*max)[0];
		if (step)
			_step = ((std::vector<float>)*step)[0];
		EVal *guitype = _param->getAnnotationValue("type");
		EVal *isSlider = _param->getAnnotationValue("is_slider");

		std::unordered_map<std::string, uint32_t> types = {
			{ "combobox", combobox },
			{ "list", list },
			{ "num", num },
			{ "slider", slider },
			{ "color" , color }
		};

		_numType = num;
		if (guitype && types.find(guitype->getString()) != types.end())
			_numType = (NumericalType)types.at(guitype->getString());
		else {
			if (isSlider && ((std::vector<bool>)*isSlider)[0])
				_numType = slider;
		}
		
		_disableProperty.reserve(_dataCount);
		_skipProperty.reserve(_dataCount);
		bool hasExpressions = false;
		for (i = 0; i < _expressions.size(); i++) {
			if (_expressions[i].empty()) {
				_disableProperty.push_back(false);
				_skipProperty.push_back(false);
				continue;
			}
			hasExpressions = true;
			_filter->compileExpression(_expressions[i]);
			if (_filter->expressionCompiled()) {
				_disableProperty.push_back(false);
				_skipProperty.push_back(true);
			} else {
				_disableProperty.push_back(true);
				_skipProperty.push_back(false);
				_tooltips[i] = _filter->expressionError();
			}
		}

		EVal *showExprLess = _param->getAnnotationValue("show_exprless");
		if (!showExprLess)
			_showExpressionLess = !hasExpressions;
		else
			_showExpressionLess = ((std::vector<bool>)*showExprLess)[0];
	}

	void getProperties(ShaderFilter *filter, obs_properties_t *props)
	{
		UNUSED_PARAMETER(filter);
		size_t i;
		if (_bind || _skipWholeProperty)
			return;
		obs_property_t *p;
		if (_isFloat) {
			if (_numType == color && _dataCount == 4) {
				obs_properties_add_color(props,
						_names[0].c_str(),
						_descs[0].c_str());
				return;
			}
			for (i = 0; i < _dataCount; i++) {
				if (_skipProperty[i])
					continue;
				if (!_showExpressionLess && _expressions[i].empty())
					continue;
				switch (_numType) {
				case combobox:
				case list:
					p = obs_properties_add_list(props,
							_names[i].c_str(),
							_descs[i].c_str(),
							OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_FLOAT);
					fillFloatList(_param, p);
					break;
				case slider:
					p = obs_properties_add_float_slider(props,
							_names[i].c_str(),
							_descs[i].c_str(), _min,
							_max, _step);
					break;
				default:
					p = obs_properties_add_float(props,
							_names[i].c_str(),
							_descs[i].c_str(), _min,
							_max, _step);
					break;
				}
				obs_property_set_enabled(p, !_disableProperty[i]);
				obs_property_set_long_description(p, _tooltips[i].c_str());
			}
		} else if(_isInt) {
			for (i = 0; i < _dataCount; i++) {
				if (_skipProperty[i])
					continue;
				if (!_showExpressionLess && _expressions[i].empty())
					continue;
				switch (_numType) {
				case combobox:
				case list:
					p = obs_properties_add_list(props,
							_names[i].c_str(),
							_descs[i].c_str(),
							OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_INT);
					fillIntList(_param, p);
					break;
				case slider:
					p = obs_properties_add_int_slider(props,
							_names[i].c_str(),
							_descs[i].c_str(),
							(int)_min, (int)_max,
							(int)_step);
					break;
				default:
					p = obs_properties_add_int(props,
							_names[i].c_str(),
							_descs[i].c_str(),
							(int)_min, (int)_max,
							(int)_step);
					break;
				}
				obs_property_set_enabled(p, !_disableProperty[i]);
				obs_property_set_long_description(p, _tooltips[i].c_str());
			}
		} else {
			for (i = 0; i < _dataCount; i++) {
				if (_skipProperty[i])
					continue;
				if (!_showExpressionLess && _expressions[i].empty())
					continue;
				switch (_numType) {
				case combobox:
				case list:
					p = obs_properties_add_list(props,
						_names[i].c_str(),
						_descs[i].c_str(),
						OBS_COMBO_TYPE_LIST,
						OBS_COMBO_FORMAT_INT);
					fillComboBox(_param, p);
					break;
				default:
					p = obs_properties_add_bool(props,
							_names[i].c_str(),
							_descs[i].c_str());
				}
				obs_property_set_enabled(p, !_disableProperty[i]);
				obs_property_set_long_description(p, _tooltips[i].c_str());
			}
		}
	}

	void update(ShaderFilter *filter)
	{
		if (_bind || _skipWholeProperty)
			return;
		obs_data_t *settings = filter->getSettings();
		size_t i;
		for (i = 0; i < _dataCount; i++) {
			switch (_paramType) {
			case GS_SHADER_PARAM_BOOL:
				switch (_numType) {
				case combobox:
				case list:
					_bindings[i].s64i = obs_data_get_int(settings,
						_names[i].c_str());
					_values[i].s32i = (int32_t)_bindings[i].s64i;
					break;
				default:
					_bindings[i].s64i = obs_data_get_bool(settings,
						_names[i].c_str());
					_values[i].s32i = (int32_t)_bindings[i].s64i;
					break;
				}
				break;
			case GS_SHADER_PARAM_INT:
			case GS_SHADER_PARAM_INT2:
			case GS_SHADER_PARAM_INT3:
			case GS_SHADER_PARAM_INT4:
				_bindings[i].s64i = obs_data_get_int(settings,
						_names[i].c_str());
				_values[i].s32i = (int32_t)_bindings[i].s64i;
				break;
			case GS_SHADER_PARAM_FLOAT:
			case GS_SHADER_PARAM_VEC2:
			case GS_SHADER_PARAM_VEC3:
			case GS_SHADER_PARAM_VEC4:
			case GS_SHADER_PARAM_MATRIX4X4:
				_bindings[i].d = obs_data_get_double(settings,
						_names[i].c_str());
				_values[i].f = (float)_bindings[i].d;
				break;
			default:
				break;
			}
		}
	}

	void videoTick(ShaderFilter *filter, float elapsedTime, float seconds)
	{
		UNUSED_PARAMETER(seconds);
		UNUSED_PARAMETER(elapsedTime);
		size_t i;
		if (_skipCalculations)
			return;
		for (i = 0; i < _dataCount; i++) {
			if (!_expressions[i].empty()) {
				switch (_paramType) {
				case GS_SHADER_PARAM_BOOL:
					_filter->compileExpression(_expressions[i]);
					_bindings[i].s64i = filter->evaluateExpression<long long>(0);
					_values[i].s32i = (int32_t)_bindings[i].s64i;
					break;
				case GS_SHADER_PARAM_INT:
				case GS_SHADER_PARAM_INT2:
				case GS_SHADER_PARAM_INT3:
				case GS_SHADER_PARAM_INT4:
					_filter->compileExpression(_expressions[i]);
					_bindings[i].s64i = filter->evaluateExpression<long long>(0);
					_values[i].s32i = (int32_t)_bindings[i].s64i;
					break;
				case GS_SHADER_PARAM_FLOAT:
				case GS_SHADER_PARAM_VEC2:
				case GS_SHADER_PARAM_VEC3:
				case GS_SHADER_PARAM_VEC4:
				case GS_SHADER_PARAM_MATRIX4X4:
					_filter->compileExpression(_expressions[i]);
					_bindings[i].d = filter->evaluateExpression<double>(0);
					_values[i].f = (float)_bindings[i].d;
					break;
				default:
					break;
				}
			} else if (_bind) {
				switch (_paramType) {
				case GS_SHADER_PARAM_BOOL:
					_bindings[i].s64i = static_cast<bool*>(_bind)[i];
					_values[i].s32i = (int32_t)_bindings[i].s64i;
					break;
				case GS_SHADER_PARAM_INT:
				case GS_SHADER_PARAM_INT2:
				case GS_SHADER_PARAM_INT3:
				case GS_SHADER_PARAM_INT4:
					_bindings[i].s64i = static_cast<int*>(_bind)[i];
					_values[i].s32i = (int32_t)_bindings[i].s64i;
					break;
				case GS_SHADER_PARAM_FLOAT:
				case GS_SHADER_PARAM_VEC2:
				case GS_SHADER_PARAM_VEC3:
				case GS_SHADER_PARAM_VEC4:
				case GS_SHADER_PARAM_MATRIX4X4:
					_bindings[i].d = static_cast<float*>(_bind)[i];
					_values[i].f = (float)_bindings[i].d;
					break;
				default:
					break;
				}
			}
		}
	}

	void setData()
	{
		if (_param) {
			if (_isFloat) {
				float *data = (float*)_values.data();
				_param->setValue<float>(data, _values.size() * sizeof(float));
			} else {
				int *data = (int*)_values.data();
				_param->setValue<int>(data, _values.size() * sizeof(int));
			}
		}
	}

	template <class DataType>
	void setData(DataType t)
	{
		if (_param)
			_param->setValue<DataType>(&t, sizeof(t));
	}

	template <class DataType>
	void setData(std::vector<DataType> t)
	{
		if (_param)
			_param->setValue<DataType>(t.data(), t.size() * sizeof(DataType));
	}

	void videoRender(ShaderFilter *filter)
	{
		UNUSED_PARAMETER(filter);
		if (_skipCalculations)
			return;
		
		setData();
	}
};

/* TODO? */
class StringData : public ShaderData {
	std::string _value;

	std::vector<std::string> _binding;
	std::vector<double> _bindings;
public:
	StringData(ShaderParameter *parent, ShaderFilter *filter) :
			ShaderData(parent, filter)
	{
	};

	~StringData()
	{
	};

	void init(gs_shader_param_type paramType)
	{
		ShaderData::init(paramType);
	}
};

/* functions to add sources to a list for use as textures */
static bool fillPropertiesSourceList(void *param, obs_source_t *source)
{
	obs_property_t *p = (obs_property_t *)param;
	uint32_t flags = obs_source_get_output_flags(source);
	const char *source_name = obs_source_get_name(source);

	if ((flags & OBS_SOURCE_VIDEO) != 0 && obs_source_active(source))
		obs_property_list_add_string(p, source_name, source_name);

	return true;
}

static void fillSourceList(obs_property_t *p)
{
	obs_property_list_add_string(p, _OMT("None"), "");
	obs_enum_sources(&fillPropertiesSourceList, (void *)p);
}

static bool fillPropertiesAudioSourceList(void *param, obs_source_t *source)
{
	obs_property_t *p = (obs_property_t *)param;
	uint32_t flags = obs_source_get_output_flags(source);
	const char *source_name = obs_source_get_name(source);

	if ((flags & OBS_SOURCE_AUDIO) != 0 && obs_source_active(source))
		obs_property_list_add_string(p, source_name, source_name);

	return true;
}

static void fillAudioSourceList(obs_property_t *p)
{
	obs_property_list_add_string(p, _OMT("None"), "");
	obs_enum_sources(&fillPropertiesAudioSourceList, (void *)p);
}

class TextureData : public ShaderData {
private:
	void renderSource(EParam *param, uint32_t cx, uint32_t cy)
	{
		if (!param)
			return;
		uint32_t media_cx = obs_source_get_width(_mediaSource);
		uint32_t media_cy = obs_source_get_height(_mediaSource);

		if (!media_cx || !media_cy)
			return;

		float scale_x = cx / (float)media_cx;
		float scale_y = cy / (float)media_cy;

		gs_texrender_reset(_texrender);
		if (gs_texrender_begin(_texrender, media_cx, media_cy)) {
			struct vec4 clear_color;
			vec4_zero(&clear_color);

			gs_clear(GS_CLEAR_COLOR, &clear_color, 1, 0);
			gs_matrix_scale3f(scale_x, scale_y, 1.0f);
			obs_source_video_render(_mediaSource);

			gs_texrender_end(_texrender);
		} else {
			return;
		}

		gs_texture_t *tex = gs_texrender_get_texture(_texrender);
		gs_effect_set_texture(*param, tex);
	}

	uint32_t processAudio(size_t samples)
	{
		size_t i;
		size_t h_samples = samples / 2;
		size_t h_sample_size = samples * 2;

		for (i = 0; i < _channels; i++) {
			audio_fft_complex(((float*)_data) + (i * samples),
				(uint32_t)samples);
		}
		for (i = 1; i < _channels; i++) {
			memcpy(((float*)_data) + (i * h_samples),
				((float*)_data) + (i * samples),
				h_sample_size);
		}
		return (uint32_t)h_samples;
	}

	void renderAudioSource(EParam *param, uint64_t samples)
	{
		if (!_data)
			_data = (uint8_t*)bzalloc(_maxAudioSize * _channels * sizeof(float));
		size_t px_width = samples;
		lock();
		size_t i = 0;
		for (i = 0; i < _channels; i++) {
			if(_audio[i].data())
				memcpy((float*)_data + (samples * i), _audio[i].data(),
						samples * sizeof(float));
			else
				memset((float*)_data + (samples * i), 0,
						samples * sizeof(float));
		}

		unlock();
		if (_isFFT)
			px_width = processAudio(samples);

		obs_enter_graphics();
		gs_texture_destroy(_tex);
		_tex = gs_texture_create((uint32_t)px_width,
				(uint32_t)_channels, GS_R32F, 1,
				(const uint8_t **)&_data, 0);
		obs_leave_graphics();
		gs_effect_set_texture(*param, _tex);
	}

	void updateAudioSource(std::string name)
	{
		lock();
		obs_source_t *sidechain = nullptr;
		if (!name.empty())
			sidechain = obs_get_source_by_name(name.c_str());
		obs_source_t *old_sidechain = _mediaSource;

		if (old_sidechain) {
			obs_source_remove_audio_capture_callback(old_sidechain,
					sidechain_capture, this);
			obs_source_release(old_sidechain);
			for (size_t i = 0; i < MAX_AV_PLANES; i++)
				_audio[i].clear();
		}
		if (sidechain)
			obs_source_add_audio_capture_callback(sidechain,
					sidechain_capture, this);
		_mediaSource = sidechain;
		unlock();
	}

	pthread_mutex_t _mutex;
protected:
	gs_texrender_t * _texrender = nullptr;
	gs_texture_t *_tex = nullptr;
	gs_image_file_t *_image = nullptr;
	std::vector<float> _audio[MAX_AV_PLANES];
	bool _isFFT = false;
	std::vector<float> _fft_data[MAX_AV_PLANES];
	size_t _channels = 0;
	size_t _maxAudioSize = AUDIO_OUTPUT_FRAMES * 2;
	uint8_t *_data = nullptr;
	obs_source_t *_mediaSource = nullptr;
	std::string _sourceName = "";
	size_t _size;
	enum TextureType {
		ignored,
		unspecified,
		source,
		audio,
		image,
		media
	};
	fft_windowing_type _window;
	TextureType _texType;
public:
	TextureData(ShaderParameter *parent, ShaderFilter *filter) :
			ShaderData(parent, filter),
			_maxAudioSize(AUDIO_OUTPUT_FRAMES * 2)
	{
		_maxAudioSize = AUDIO_OUTPUT_FRAMES * 2;
		if (pthread_mutex_init(&_mutex, NULL) != 0)
			blog(LOG_INFO, "");
	};

	~TextureData()
	{
		if(_texType == audio)
			obs_source_remove_audio_capture_callback(_mediaSource,
					sidechain_capture, this);
		obs_enter_graphics();
		gs_texrender_destroy(_texrender);
		gs_image_file_free(_image);
		gs_texture_destroy(_tex);
		obs_leave_graphics();
		if (_data)
			bfree(_data);
		pthread_mutex_destroy(&_mutex);
	};

	void lock()
	{
		pthread_mutex_lock(&_mutex);
	}

	void unlock()
	{
		pthread_mutex_unlock(&_mutex);
	}

	size_t getAudioChannels()
	{
		return _channels;
	}

	void insertAudio(float* data, size_t samples, size_t index)
	{
		if (!samples || index > (MAX_AV_PLANES - 1))
			return;
		size_t old_size = _audio[index].size() * sizeof(float);
		size_t insert_size = samples * sizeof(float);
		float* old_data = nullptr;
		if (old_size)
			old_data = (float*)bmemdup(_audio[index].data(), old_size);
		_audio[index].resize(_maxAudioSize);
		if (samples < _maxAudioSize) {
			if (old_data)
				memcpy(&_audio[index][samples], old_data, old_size - insert_size);
			if (data)
				memcpy(&_audio[index][0], data, insert_size);
			else
				memset(&_audio[index][0], 0, insert_size);
		} else {
			if (data)
				memcpy(&_audio[index][0], data,
					_maxAudioSize * sizeof(float));
			else
				memset(&_audio[index][0], 0,
					_maxAudioSize * sizeof(float));
		}
		bfree(old_data);
	}

	void init(gs_shader_param_type paramType)
	{
		_paramType = paramType;
		_names.push_back(_parent->getName());
		_descs.push_back(_parent->getDescription());

		EVal *texType = _param->getAnnotationValue("texture_type");
		std::unordered_map<std::string, uint32_t> types = {
			{"source", source},
			{"audio", audio},
			{"image", image},
			{"media", media}
		};

		if (texType && types.find(texType->getString()) != types.end()) {
			_texType = (TextureType)types.at(texType->getString());
		} else {
			_texType = image;
		}

		if (_names[0] == "image")
			_texType = ignored;

		_channels = audio_output_get_channels(obs_get_audio());
		if (_texType == audio) {
			EVal *channels = _param->getAnnotationValue("channels");
			if (channels)
				_channels = ((std::vector<int>)*channels)[0];

			for(size_t i = 0; i < MAX_AV_PLANES; i++)
				_audio->resize(AUDIO_OUTPUT_FRAMES);

			EVal *fft = _param->getAnnotationValue("is_fft");
			if (fft)
				_isFFT = ((std::vector<bool>)*fft)[0];
			else
				_isFFT = false;

			EVal *window = _param->getAnnotationValue("window");
			if (window)
				_window = get_window_type(window->c_str());
			else
				_window = none;
		}
	}

	void getProperties(ShaderFilter *filter, obs_properties_t *props)
	{
		UNUSED_PARAMETER(filter);
		obs_property_t *p = nullptr;
		switch (_texType) {
		case source:
			p = obs_properties_add_list(props, _names[0].c_str(),
					_descs[0].c_str(), OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
			fillSourceList(p);
			break;
		case audio:
			p = obs_properties_add_list(props, _names[0].c_str(),
					_descs[0].c_str(), OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
			fillAudioSourceList(p);
			break;
		case media:
			p = obs_properties_add_path(props, _names[0].c_str(),
					_descs[0].c_str(), OBS_PATH_FILE,
					shader_filter_media_file_filter,
					NULL);
			break;
		case image:
			p = obs_properties_add_path(props, _names[0].c_str(),
					_descs[0].c_str(), OBS_PATH_FILE,
					shader_filter_texture_file_filter,
					NULL);
			break;
		}
	}

	void update(ShaderFilter *filter)
	{
		obs_data_t *settings = filter->getSettings();
		_channels = audio_output_get_channels(obs_get_audio());
		switch (_texType) {
		case source:
			if (!_texrender)
				_texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			obs_source_release(_mediaSource);
			_mediaSource = obs_get_source_by_name(
					obs_data_get_string(settings,
					_names[0].c_str()));
			break;
		case audio:
			updateAudioSource(obs_data_get_string(settings,
					_names[0].c_str()));
			break;
		case image:
			if (!_image) {
				_image = (gs_image_file_t *)bzalloc(sizeof(gs_image_file_t));
			} else {
				obs_enter_graphics();
				gs_image_file_free(_image);
				obs_leave_graphics();
			}
			gs_image_file_init(_image, obs_data_get_string(settings,
					_names[0].c_str()));
			obs_enter_graphics();
			gs_image_file_init_texture(_image);
			obs_leave_graphics();
			break;
		}
	}

	void videoRender(ShaderFilter *filter)
	{
		uint32_t src_cx = obs_source_get_width(filter->context);
		uint32_t src_cy = obs_source_get_height(filter->context);
		EParam *e = _parent->getParameter();
		gs_texture_t *t;
		switch (_texType) {
		case media:
		case source:
			renderSource(e, src_cx, src_cy);
			break;
		case audio:
			renderAudioSource(e, AUDIO_OUTPUT_FRAMES);
			break;
		case image:
			if (_image)
				t = _image->texture;
			else
				t = nullptr;
			e->setValue<gs_texture_t*>(&t, sizeof(gs_texture_t*));
			break;
		default:
			break;
		}
	}
};

static void sidechain_capture(void *p, obs_source_t *source,
		const struct audio_data *audio_data, bool muted)
{
	TextureData *data = static_cast<TextureData *>(p);

	UNUSED_PARAMETER(source);

	size_t i;
	data->lock();
	if (muted) {
		for (i = 0; i < data->getAudioChannels(); i++)
			data->insertAudio(nullptr, audio_data->frames, i);
	} else {
		for (i = 0; i < data->getAudioChannels(); i++)
			data->insertAudio((float*)audio_data->data[i],
					audio_data->frames, i);
	}
	data->unlock();
}

class NullData : public ShaderData {
public:
	NullData(ShaderParameter *parent, ShaderFilter *filter) :
			ShaderData(parent, filter)
	{
	};
	~NullData()
	{
	};
	void init(gs_shader_param_type paramType)
	{
		UNUSED_PARAMETER(paramType);
	}
};


std::string ShaderParameter::getName()
{
	return _name;
}

std::string ShaderParameter::getDescription()
{
	return _description;
}

EParam *ShaderParameter::getParameter()
{
	return _param;
}

ShaderParameter::ShaderParameter(gs_eparam_t *param, ShaderFilter *filter) :
		_filter(filter)
{
	struct gs_effect_param_info info;
	gs_effect_get_param_info(param, &info);

	pthread_mutexattr_t attr;
	if (pthread_mutexattr_init(&attr) != 0) {
		_mutex_created = false;
		return;
	}
	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0) {
		_mutex_created = false;
		return;
	}

	_mutex_created = pthread_mutex_init(&_mutex, NULL) == 0;
	_name = info.name;
	_description = info.name;
	_param = new EParam(param);

	init(info.type);
}

void ShaderParameter::init(gs_shader_param_type paramType)
{
	lock();
	_paramType = paramType;
	switch (paramType) {
	case GS_SHADER_PARAM_BOOL:
	case GS_SHADER_PARAM_INT:
	case GS_SHADER_PARAM_INT2:
	case GS_SHADER_PARAM_INT3:
	case GS_SHADER_PARAM_INT4:
	case GS_SHADER_PARAM_FLOAT:
	case GS_SHADER_PARAM_VEC2:
	case GS_SHADER_PARAM_VEC3:
	case GS_SHADER_PARAM_VEC4:
	case GS_SHADER_PARAM_MATRIX4X4:
		_shaderData = new NumericalData(this, _filter);
		break;
	case GS_SHADER_PARAM_TEXTURE:
		_shaderData = new TextureData(this, _filter);
		break;
	case GS_SHADER_PARAM_STRING:
		_shaderData = new StringData(this, _filter);
		break;
	case GS_SHADER_PARAM_UNKNOWN:
		_shaderData = new NullData(this, _filter);
		break;
	}
	if (_shaderData)
		_shaderData->init(paramType);
	unlock();
}

ShaderParameter::~ShaderParameter()
{
	if (_mutex_created)
		pthread_mutex_destroy(&_mutex);

	if (_param)
		delete _param;

	if (_shaderData)
		delete _shaderData;
}

void ShaderParameter::lock()
{
	if (_mutex_created)
		pthread_mutex_lock(&_mutex);
}

void ShaderParameter::unlock()
{
	if (_mutex_created)
		pthread_mutex_unlock(&_mutex);
}

void ShaderParameter::videoTick(ShaderFilter *filter, float elapsed_time, float seconds)
{
	lock();
	if (_shaderData)
		_shaderData->videoTick(filter, elapsed_time, seconds);
	unlock();
}

void ShaderParameter::videoRender(ShaderFilter *filter)
{
	lock();
	if (_shaderData)
		_shaderData->videoRender(filter);
	unlock();
}

void ShaderParameter::update(ShaderFilter *filter)
{
	lock();
	if (_shaderData)
		_shaderData->update(filter);
	unlock();
}

void ShaderParameter::getProperties(ShaderFilter *filter, obs_properties_t *props)
{
	lock();
	if (_shaderData)
		_shaderData->getProperties(filter, props);
	unlock();
}

obs_data_t *ShaderFilter::getSettings()
{
	return _settings;
}

std::string ShaderFilter::getPath()
{
	return _effect_path;
}

void ShaderFilter::setPath(std::string path)
{
	_effect_path = path;
}

void ShaderFilter::prepReload()
{
	_reload_effect = true;
}

bool ShaderFilter::needsReloading()
{
	return _reload_effect;
}

std::vector<ShaderParameter*> ShaderFilter::parameters()
{
	return paramList;
}

void ShaderFilter::clearExpression()
{
	expression.clear();
}

void ShaderFilter::appendVariable(te_variable var)
{
	expression.push_back(var);
}

void ShaderFilter::compileExpression(std::string expresion)
{
	expression.compile(expresion);
}

bool ShaderFilter::expressionCompiled()
{
	return expression;
}

std::string ShaderFilter::expressionError()
{
	return expression.errorString();
}

template <class DataType>
DataType ShaderFilter::evaluateExpression(DataType default_value)
{
	return expression.evaluate(default_value);
}

ShaderFilter::ShaderFilter(obs_data_t *settings, obs_source_t *source)
{
	context = source;
	_settings = settings;
	pthread_mutexattr_t attr;
	if (pthread_mutexattr_init(&attr) != 0) {
		_mutex_created = false;
		return;
	}
	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0) {
		_mutex_created = false;
		return;
	}
	_mutex_created = pthread_mutex_init(&_mutex, NULL) == 0;
	prepReload();
	update(this, _settings);
};

ShaderFilter::~ShaderFilter()
{
	/* Clear previous settings */
	while (!paramList.empty()) {
		ShaderParameter *p = paramList.back();
		paramList.pop_back();
		delete p;
	}

	obs_enter_graphics();
	gs_effect_destroy(effect);
	effect = nullptr;
	obs_leave_graphics();
};

void ShaderFilter::lock()
{
	if (_mutex_created)
		pthread_mutex_lock(&_mutex);
}

void ShaderFilter::unlock()
{
	if (_mutex_created)
		pthread_mutex_unlock(&_mutex);
}

uint32_t ShaderFilter::getWidth()
{
	return total_width;
}
uint32_t ShaderFilter::getHeight()
{
	return total_height;
}

void ShaderFilter::updateCache(gs_eparam_t *param)
{
	ShaderParameter *p = new ShaderParameter(param, this);
	if(p)
		paramList.push_back(p);
}

void ShaderFilter::reload()
{
	_reload_effect = false;
	size_t i;
	char *errors = NULL;

	/* Clear previous settings */
	while (!paramList.empty()) {
		ShaderParameter *p = paramList.back();
		paramList.pop_back();
		delete p;
	}

	evaluationList.clear();
	expression.clear();

	prepFunctions(&expression);

	obs_enter_graphics();
	gs_effect_destroy(effect);
	effect = nullptr;
	obs_leave_graphics();

	_effect_path = obs_data_get_string(_settings, "shader_file_name");
	/* Load default effect text if no file is selected */
	char *effect_string = nullptr;
	if (!_effect_path.empty())
		effect_string = os_quick_read_utf8_file(_effect_path.c_str());
	else
		return;

	obs_enter_graphics();
	effect = gs_effect_create(effect_string, NULL, &errors);
	obs_leave_graphics();

	_effect_string = effect_string;
	bfree(effect_string);

	/* Create new parameters */
	size_t effect_count = gs_effect_get_num_params(effect);
	paramList.reserve(effect_count);
	for (i = 0; i < effect_count; i++) {
		gs_eparam_t *param = gs_effect_get_param_by_idx(effect, i);
		updateCache(param);
	}
}

void *ShaderFilter::create(obs_data_t *settings, obs_source_t *source)
{
	ShaderFilter *filter = new ShaderFilter(settings, source);
	return filter;
}

void ShaderFilter::destroy(void *data)
{
	delete static_cast<ShaderFilter*>(data);
}

const char *ShaderFilter::getName(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ShaderFilter");
}

void ShaderFilter::videoTick(void *data, float seconds)
{
	ShaderFilter *filter = static_cast<ShaderFilter*>(data);
	filter->elapsedTimeBinding.d += seconds;
	filter->elapsedTime += seconds;

	size_t i;
	std::vector<ShaderParameter*> parameters = filter->parameters();
	for (i = 0; i < parameters.size(); i++) {
		if (parameters[i])
			parameters[i]->videoTick(filter, filter->elapsedTime,
					seconds);
	}

	int *resize[4] = {&filter->resizeLeft, &filter->resizeRight,
			&filter->resizeTop, &filter->resizeBottom};
	for (i = 0; i < 4; i++) {
		if (filter->resizeExpressions[i].empty())
			continue;
		filter->compileExpression(filter->resizeExpressions[i]);
		if (filter->expressionCompiled())
			*resize[i] = filter->evaluateExpression<int>(0);
	}

	obs_source_t *target = obs_filter_get_target(filter->context);
	/* Determine offsets from expansion values. */
	int baseWidth = obs_source_get_base_width(target);
	int baseHeight = obs_source_get_base_height(target);

	filter->total_width = filter->resizeLeft + baseWidth +
			filter->resizeRight;
	filter->total_height = filter->resizeTop + baseHeight +
			filter->resizeBottom;

	filter->uvScale.x = (float)filter->total_width / baseWidth;
	filter->uvScale.y = (float)filter->total_height / baseHeight;
	filter->uvOffset.x = (float)(-filter->resizeLeft) / baseWidth;
	filter->uvOffset.y = (float)(-filter->resizeTop) / baseHeight;
	filter->uvPixelInterval.x = 1.0f / baseWidth;
	filter->uvPixelInterval.y = 1.0f / baseHeight;

	filter->uvScaleBinding = filter->uvScale;
	filter->uvOffsetBinding = filter->uvOffset;
}

void ShaderFilter::videoRender(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	ShaderFilter *filter = static_cast<ShaderFilter*>(data);
	size_t i;

	if (filter->effect != nullptr) {
		if (!obs_source_process_filter_begin(filter->context,
			GS_RGBA, OBS_NO_DIRECT_RENDERING))
			return;

		std::vector<ShaderParameter*> parameters = filter->parameters();

		for (i = 0; i < parameters.size(); i++) {
			if (parameters[i])
				parameters[i]->videoRender(filter);
		}
		
		obs_source_process_filter_end(filter->context, filter->effect,
				filter->total_width, filter->total_height);
	} else {
		obs_source_skip_video_filter(filter->context);
	}
}

void ShaderFilter::update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	ShaderFilter *filter = static_cast<ShaderFilter*>(data);
	if (filter->needsReloading()) {
		filter->reload();
		obs_source_update_properties(filter->context);
	}
	size_t i;
	std::vector<ShaderParameter*> parameters = filter->parameters();
	for (i = 0; i < parameters.size(); i++) {
		if (parameters[i])
			parameters[i]->update(filter);
	}
}

obs_properties_t *ShaderFilter::getProperties(void *data)
{
	ShaderFilter *filter = static_cast<ShaderFilter*>(data);
	size_t i;
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_param(props, filter, NULL);

	std::string shaderPath = obs_get_module_data_path(obs_current_module());
	shaderPath += "/shaders";

	obs_properties_add_button(props, "reload_effect",
		_OMT("ShaderFilter.ReloadEffect"),
		shader_filter_reload_effect_clicked);

	obs_property_t *file_name = obs_properties_add_path(props,
			"shader_file_name", _OMT("ShaderFilter.ShaderFileName"),
			OBS_PATH_FILE, NULL, shaderPath.c_str());

	obs_property_set_modified_callback(file_name,
			shader_filter_file_name_changed);

	std::vector<ShaderParameter*> parameters = filter->parameters();
	for (i = 0; i < parameters.size(); i++) {
		if (parameters[i])
			parameters[i]->getProperties(filter, props);
	}
	return props;
}

uint32_t ShaderFilter::getWidth(void *data)
{
	ShaderFilter *filter = static_cast<ShaderFilter*>(data);
	return filter->getWidth();
}

uint32_t ShaderFilter::getHeight(void *data)
{
	ShaderFilter *filter = static_cast<ShaderFilter*>(data);
	return filter->getHeight();
}

void ShaderFilter::getDefaults(obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
}

static bool shader_filter_reload_effect_clicked(obs_properties_t *props,
	obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(property);
	UNUSED_PARAMETER(props);
	ShaderFilter *filter = static_cast<ShaderFilter*>(data);
	filter->prepReload();
	obs_source_update(filter->context, NULL);

	return true;
}

static bool shader_filter_file_name_changed(obs_properties_t *props,
	obs_property_t *p, obs_data_t *settings)
{
	ShaderFilter *filter = static_cast<ShaderFilter*>(
		obs_properties_get_param(props));
	std::string path = obs_data_get_string(settings, obs_property_name(p));

	if (filter->getPath() != path) {
		filter->prepReload();
		filter->setPath(path);
		obs_source_update(filter->context, NULL);
	}

	return true;
}

bool obs_module_load(void)
{
	struct obs_source_info shader_filter = { 0 };
	shader_filter.id = "obs_shader_filter";
	shader_filter.type = OBS_SOURCE_TYPE_FILTER;
	shader_filter.output_flags = OBS_SOURCE_VIDEO;
	shader_filter.get_name = ShaderFilter::getName;
	shader_filter.create = ShaderFilter::create;
	shader_filter.destroy = ShaderFilter::destroy;
	shader_filter.update = ShaderFilter::update;
	shader_filter.video_tick = ShaderFilter::videoTick;
	shader_filter.video_render = ShaderFilter::videoRender;
	shader_filter.get_defaults = ShaderFilter::getDefaults;
	shader_filter.get_width = ShaderFilter::getWidth;
	shader_filter.get_height = ShaderFilter::getHeight;
	shader_filter.get_properties = ShaderFilter::getProperties;

	obs_register_source(&shader_filter);

	struct obs_audio_info aoi;
	obs_get_audio_info(&aoi);
	sample_rate = (double)aoi.samples_per_sec;
	output_channels = (double)get_audio_channels(aoi.speakers);

	return true;
}

void obs_module_unload(void)
{
}
