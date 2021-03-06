// **********************************************************************
//
// Copyright (c) 2003-2017 ZeroC, Inc. All rights reserved.
//
// This copy of Ice is licensed to you under the terms described in the
// ICE_LICENSE file included in this distribution.
//
// **********************************************************************

using System;

public class Server : TestCommon.Application
{
    public override int run(string[] args)
    {
        communicator().getProperties().setProperty("TestAdapter.Endpoints", getTestEndpoint(0) + " -t 2000");
        Ice.ObjectAdapter adapter = communicator().createObjectAdapter("TestAdapter");
        Ice.Object @object = new Test.ChecksumI();
        adapter.add(@object, Ice.Util.stringToIdentity("test"));
        adapter.activate();
        communicator().waitForShutdown();
        return 0;
    }

    public static int Main(string[] args)
    {
        Server app = new Server();
        return app.runmain(args);
    }
}
