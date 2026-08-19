static int constructor_value;
static __thread int fixture_tls = 29;

static void __attribute__((constructor)) fixture_constructor(void)
{
	constructor_value = 13;
}

int dynamic_fixture_value(void)
{
	return 17;
}

int dynamic_fixture_constructor_value(void)
{
	return constructor_value;
}

int dynamic_fixture_tls_value(void)
{
	return fixture_tls;
}
