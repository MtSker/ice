// **********************************************************************
//
// Copyright (c) 2003-2016 ZeroC, Inc. All rights reserved.
//
// This copy of Ice is licensed to you under the terms described in the
// ICE_LICENSE file included in this distribution.
//
// **********************************************************************
package test.Ice.background;

final class EndpointFactory implements IceInternal.EndpointFactory
{
    EndpointFactory(Configuration configuration, IceInternal.EndpointFactory factory)
    {
        _configuration = configuration;
        _factory = factory;
    }

    @Override
    public short
    type()
    {
        return (short)(EndpointI.TYPE_BASE + _factory.type());
    }

    @Override
    public String
    protocol()
    {
        return "test-" + _factory.protocol();
    }

    @Override
    public IceInternal.EndpointI
    create(java.util.ArrayList<String> args, boolean server)
    {
        return new EndpointI(_configuration, _factory.create(args, server));
    }

    @Override
    public IceInternal.EndpointI
    read(Ice.InputStream s)
    {
        short type = s.readShort();
        assert(type == _factory.type());

        s.startEncapsulation();
        IceInternal.EndpointI endpoint = new EndpointI(_configuration, _factory.read(s));
        s.endEncapsulation();
        return endpoint;
    }

    @Override
    public void
    destroy()
    {
    }

    @Override
    public IceInternal.EndpointFactory
    clone(IceInternal.ProtocolInstance instance, IceInternal.EndpointFactory del)
    {
        return this;
    }

    private Configuration _configuration;
    private IceInternal.EndpointFactory _factory;
}
